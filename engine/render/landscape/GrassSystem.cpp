#include "engine/render/landscape/GrassSystem.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>
#include <unordered_map>

#include "engine/core/Hash.hpp"
#include "engine/core/Math.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/render/landscape/GrassSpecies.hpp"
#include "engine/render/landscape/SplatTextures.hpp"
#include "engine/render/landscape/VegetationSystem.hpp" // forestMask
#include "engine/render/landscape/TerrainSystem.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"
#include "engine/render/PipelinePresets.hpp"

namespace render {

namespace {

constexpr const char* kGrassShader = "grass";

// BotW-style patch layout: grass gathers in dense clumps with bare meadow
// between them. Two noise scales — broad patches and small clump detail —
// thresholded into a [0,1] density mask (0 = bare, 1 = heart of a patch).
// Every knob lives in GrassScatterTuning (render panel, "Grass").
f32 patchMask(u32 seed, const GrassScatterTuning& tuning, f32 x, f32 z) {
    const f32 broad = terrain::noise01(seed ^ 0x1f123bb5u,
                                       x / tuning.patchBroadScale,
                                       z / tuning.patchBroadScale);
    const f32 detail = terrain::noise01(seed ^ 0x9d2c5680u,
                                        x / tuning.patchDetailScale,
                                        z / tuning.patchDetailScale);
    return glm::smoothstep(tuning.patchThresholdLo, tuning.patchThresholdHi,
                           broad * 0.72f + detail * 0.28f);
}

// hashU32 / HashRng live in engine/core/Hash.hpp (shared scatter hash family).
using core::hashU32;
using core::HashRng;

// The root albedo rides the instance's spare lane (groundNormal.w) as
// packed display-space bytes; grass.vert unpacks with
// unpackUnorm4x8(floatBitsToUint(...)) and sRGB-decodes like the
// terrain's sampler. Alpha byte stays 0 — the bit pattern can never be
// a NaN/Inf, so the f32 attribute fetch is lossless.
f32 packAlbedo(const Vec3& color) {
    const u32 r = static_cast<u32>(glm::clamp(color.r, 0.0f, 1.0f) * 255.0f);
    const u32 g = static_cast<u32>(glm::clamp(color.g, 0.0f, 1.0f) * 255.0f);
    const u32 b = static_cast<u32>(glm::clamp(color.b, 0.0f, 1.0f) * 255.0f);
    return std::bit_cast<f32>(r | (g << 8) | (b << 16));
}

// One blade (the SimonDev Quick_Grass model): 6 straight
// segments, sides at ±1 with NO baked taper — grass.vert shapes the
// 1-t^2 taper and the forward arc per vertex. 13 vertices, 11 triangles;
// one-sided strip, cull off (the fragment normals are ground-dominated,
// no back-face flip needed).
constexpr f32 kBladeVertices[] = {
    // side,  t
    -1.0f, 0.0f,       1.0f, 0.0f,
    -1.0f, 1.0f/6.0f,  1.0f, 1.0f/6.0f,
    -1.0f, 2.0f/6.0f,  1.0f, 2.0f/6.0f,
    -1.0f, 3.0f/6.0f,  1.0f, 3.0f/6.0f,
    -1.0f, 4.0f/6.0f,  1.0f, 4.0f/6.0f,
    -1.0f, 5.0f/6.0f,  1.0f, 5.0f/6.0f,
     0.0f, 1.0f, // the tip
};
constexpr u16 kBladeIndices[] = {
    0, 1, 2,   1, 3, 2,   2, 3, 4,   3, 5, 4,
    4, 5, 6,   5, 7, 6,   6, 7, 8,   7, 9, 8,
    8, 9, 10,  9, 11, 10, 10, 11, 12,
};


// ---------------------------------------------------------------------------
// scatterGrass pieces. The scatter is CELL-MAJOR: per-candidate noise
// evals on the fine blade grid would saturate every worker for seconds
// at boot. The masks vary over METERS, not centimeters — patch, material
// and normal are evaluated once per cell, and the terrain height on the
// cell-corner lattice (bilinear per blade — at or below the render
// mesh's own sampling error). Bare cells cost two noise evals total;
// only the cheap per-blade jitter runs per candidate.
// ---------------------------------------------------------------------------

// The chunk being scattered: world origin, candidate spacing, the cell
// lattice the per-cell masks are evaluated on, and the chunk-scoped
// water subset.
struct ChunkFrame {
    static constexpr u32 kCell = 4; // candidates per cell side (0.6 m cells)

    i32 cx { 0 };
    i32 cz { 0 };
    f32 originX { 0.0f };
    f32 originZ { 0.0f };
    f32 spacing { 0.0f };  // meters between candidates
    u32 perSide { 0 };     // candidates per chunk side (the fine grid)
    u32 cells { 0 };       // cells per chunk side
    f32 cellSize { 0.0f }; // meters
    // ±8 m slack covers the corner-lattice overshoot — bit-identical to
    // the full scan (WaterBodies.hpp).
    terrain::WaterBodiesSubset water;

    static ChunkFrame build(const TerrainParams& params,
                            const GrassScatterTuning& tuning, i32 cx,
                            i32 cz) {
        ChunkFrame frame;
        frame.cx = cx;
        frame.cz = cz;
        frame.originX = static_cast<f32>(cx) * TerrainSystem::kChunkSize;
        frame.originZ = static_cast<f32>(cz) * TerrainSystem::kChunkSize;
        frame.spacing = glm::max(0.05f, tuning.spacing);
        frame.perSide =
            static_cast<u32>(TerrainSystem::kChunkSize / frame.spacing);
        frame.cells = (frame.perSide + kCell - 1) / kCell;
        frame.cellSize = frame.spacing * static_cast<f32>(kCell);
        if (params.water) {
            frame.water = terrain::waterBodiesInRect(
                *params.water, frame.originX - 8.0f, frame.originZ - 8.0f,
                frame.originX + TerrainSystem::kChunkSize + 8.0f,
                frame.originZ + TerrainSystem::kChunkSize + 8.0f);
        }
        return frame;
    }

    // World position of a cell's min corner and center.
    f32 cellMinX(u32 cgx) const {
        return originX + static_cast<f32>(cgx) * cellSize;
    }
    f32 cellMinZ(u32 cgz) const {
        return originZ + static_cast<f32>(cgz) * cellSize;
    }
    f32 cellCenterX(u32 cgx) const {
        return originX + (static_cast<f32>(cgx) + 0.5f) * cellSize;
    }
    f32 cellCenterZ(u32 cgz) const {
        return originZ + (static_cast<f32>(cgz) + 0.5f) * cellSize;
    }
};

// Per-corner terrain samples on the cell lattice — height and root
// albedo — read bilinearly per blade. The ground albedo rides the same
// lattice: the blotch field varies over meters, and per-candidate FBM
// evals are exactly what the cell-major layout exists to avoid. Each
// corner wraps its own uv — the tile is seamless, so interpolating the
// COLORS across a tile boundary stays continuous.
struct CornerLattice {
    u32 cells { 0 };
    vector<f32> height; // (cells + 1)^2, row-major along z
    vector<Vec3> albedo;

    // One cell's four corners in bilinear order: x0z0, x1z0, x0z1, x1z1.
    struct Corners {
        f32 h00, h10, h01, h11;
        Vec3 a00, a10, a01, a11;
    };
    Corners corners(u32 cgx, u32 cgz) const {
        const u32 i00 = cgz * (cells + 1) + cgx;
        const u32 i01 = (cgz + 1) * (cells + 1) + cgx;
        return { height[i00], height[i00 + 1], height[i01], height[i01 + 1],
                 albedo[i00], albedo[i00 + 1], albedo[i01], albedo[i01 + 1] };
    }

    static CornerLattice build(const TerrainParams& params,
                               const GrassScatterTuning& tuning,
                               const ChunkFrame& frame) {
        const u32 cells = frame.cells;
        const f32 cellSize = frame.cellSize;
        const f32 originX = frame.originX;
        const f32 originZ = frame.originZ;
        CornerLattice lattice;
        lattice.cells = cells;
        lattice.height.resize((cells + 1) * (cells + 1));
        lattice.albedo.resize((cells + 1) * (cells + 1));

        // Coarse tint sub-lattice (~8 m): regionShadingAt drifts over
        // ~700 m — one eval per coarse point, bilerped to the corners,
        // instead of one per 0.6 m corner (GrassScatterTuning::coarseTint;
        // OFF = the exact per-corner reference).
        const f32 cornerSpan = static_cast<f32>(cells) * cellSize;
        const u32 coarseN =
            glm::max(2u, static_cast<u32>(cornerSpan / 8.0f) + 2u);
        const f32 coarseStep = cornerSpan / static_cast<f32>(coarseN - 1);
        vector<Vec3> coarseTint;
        if (tuning.coarseTint) {
            coarseTint.resize(static_cast<size_t>(coarseN) * coarseN);
            for (u32 gz = 0; gz < coarseN; ++gz) {
                for (u32 gx = 0; gx < coarseN; ++gx) {
                    coarseTint[static_cast<size_t>(gz) * coarseN + gx] =
                        terrain::regionShadingAt(
                            params,
                            originX + static_cast<f32>(gx) * coarseStep,
                            originZ + static_cast<f32>(gz) * coarseStep)
                            .tint;
                }
            }
        }
        const auto tintAt = [&](f32 wx, f32 wz) {
            if (!tuning.coarseTint) {
                return terrain::regionShadingAt(params, wx, wz).tint;
            }
            const f32 u =
                glm::clamp((wx - originX) / coarseStep, 0.0f,
                           static_cast<f32>(coarseN - 1));
            const f32 v =
                glm::clamp((wz - originZ) / coarseStep, 0.0f,
                           static_cast<f32>(coarseN - 1));
            const u32 u0 = glm::min(static_cast<u32>(u), coarseN - 2);
            const u32 v0 = glm::min(static_cast<u32>(v), coarseN - 2);
            const f32 tu = u - static_cast<f32>(u0);
            const f32 tv = v - static_cast<f32>(v0);
            const auto at = [&](u32 col, u32 row) {
                return coarseTint[static_cast<size_t>(row) * coarseN + col];
            };
            const Vec3 a = glm::mix(at(u0, v0), at(u0 + 1, v0), tu);
            const Vec3 b = glm::mix(at(u0, v0 + 1), at(u0 + 1, v0 + 1), tu);
            return glm::mix(a, b, tv);
        };
        for (u32 gz = 0; gz <= cells; ++gz) {
            for (u32 gx = 0; gx <= cells; ++gx) {
                const f32 wx = originX + static_cast<f32>(gx) * cellSize;
                const f32 wz = originZ + static_cast<f32>(gz) * cellSize;
                lattice.height[gz * (cells + 1) + gx] =
                    terrain::height(params, wx, wz);
                const f32 su = wx * tuning.splatUvScale;
                const f32 sv = wz * tuning.splatUvScale;
                // Macro tint applies to the root color exactly as the
                // terrain shader applies it to the ground (same
                // regionShadingAt, same strength lerp) — meadow and
                // terrain keep ONE color source. The base color is the
                // ACTIVE set's mean for the corner's ground variant
                // (grass zones), so the raccord holds on the cooked set
                // and across zone borders alike.
                const Vec3 tint = glm::mix(Vec3 { 1.0f }, tintAt(wx, wz),
                                           tuning.tintStrength);
                const Vec3 base =
                    tuning.rootAlbedoBase[terrain::grassZoneAt(wx, wz)
                                              .variantA];
                lattice.albedo[gz * (cells + 1) + gx] =
                    base *
                    grassBlotch(su - std::floor(su), sv - std::floor(sv)) *
                    tint;
            }
        }
        return lattice;
    }
};

// Ground-variant bias (terrain_zones — v0 grass, v1 worn path, v2 stones,
// v3 dirt): what the ground texture shows and what grows on it agree —
// wear and stones thin the meadow, dirt turns strawy, worn ground grows
// moss plates. Species columns in GrassSpeciesId order (hand-tuned).
constexpr f32 kZoneSpeciesBias[kGrassVariantCount][kGrassSpeciesCount] = {
    //  meadow  dry    oat    flower moss   lichen
    { 1.25f, 1.0f,  1.0f,  1.0f,  1.0f,  1.0f }, // grass
    { 0.6f,  1.0f,  0.8f,  0.7f,  1.5f,  1.0f }, // worn path
    { 0.75f, 1.3f,  1.0f,  1.0f,  1.0f,  1.0f }, // stones
    { 0.55f, 1.5f,  1.0f,  1.0f,  1.4f,  1.0f }, // dirt
};

// Species per Voronoi clump (jittered grid, ~2.4 m): neighboring blades
// share species/shade and fan away from their clump center — patches
// read as vegetation, not per-blade noise (the GoT model,
// docs/PAYSAGE.md §1.7). Sites are resolved per blade (nearest of the
// 3x3 neighbor sites — pure arithmetic) and their pick is cached per
// chunk build; per-species low-frequency noises make one species win
// over several clumps at a time.
class ClumpPicker {
public:
    static constexpr f32 kClumpSize = 2.4f;

    struct Clump {
        u8 species { 0 };
        f32 shade { 1.0f };
        Vec2 center {};
    };

    explicit ClumpPicker(const TerrainParams& terrainParams)
        : params(terrainParams) {}

    // The clump owning (x, z): nearest of the 3x3 neighbor sites, its
    // species resolved on first visit from the fields the caller sampled
    // at the cell and the cell's patch presence.
    const Clump& pick(f32 x, f32 z, const terrain::RegionFields& fields,
                      f32 presence) {
        const i32 bx = static_cast<i32>(std::floor(x / kClumpSize));
        const i32 bz = static_cast<i32>(std::floor(z / kClumpSize));
        i32 bestX = bx;
        i32 bestZ = bz;
        Vec2 bestSite {};
        f32 bestD = 1e9f;
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                const Vec2 candidate = site(bx + dx, bz + dz);
                const f32 d = (candidate.x - x) * (candidate.x - x) +
                              (candidate.y - z) * (candidate.y - z);
                if (d < bestD) {
                    bestD = d;
                    bestX = bx + dx;
                    bestZ = bz + dz;
                    bestSite = candidate;
                }
            }
        }
        const u64 key =
            (static_cast<u64>(static_cast<u32>(bestX)) << 32) |
            static_cast<u32>(bestZ);
        const auto it = clumps.find(key);
        if (it != clumps.end()) {
            return it->second;
        }
        return clumps.emplace(key, resolve(key, bestSite, fields, presence))
            .first->second;
    }

private:
    // Jittered site of the clump grid cell (gx, gz), in world meters.
    Vec2 site(i32 gx, i32 gz) const {
        const u32 h = hashU32(params.seed ^ 0x51c7a3b9u ^
                              hashU32(static_cast<u32>(gx) * 0x9e3779b9u ^
                                      static_cast<u32>(gz) * 0x85ebca6bu));
        const f32 jx = static_cast<f32>(h & 0xffffu) * (1.0f / 65535.0f);
        const f32 jz = static_cast<f32>(h >> 16) * (1.0f / 65535.0f);
        return Vec2 { (static_cast<f32>(gx) + 0.15f + 0.7f * jx) *
                          kClumpSize,
                      (static_cast<f32>(gz) + 0.15f + 0.7f * jz) *
                          kClumpSize };
    }

    // Species scores at the winning site. Aridity mirrors the macro
    // tint's climate read (temperature/biomeWetness), so dry straw
    // grows where the ground reads parched.
    Clump resolve(u64 key, const Vec2& center,
                  const terrain::RegionFields& fields, f32 presence) const {
        const f32 inv = 1.0f / 9.0f; // ~9 m species patches
        const f32 dryBias =
            glm::clamp(0.5f + 0.35f * fields.temperature -
                           0.4f * fields.biomeWetness,
                       0.0f, 1.0f);
        const f32 n0 = terrain::noise01(params.seed ^ 0x0a17u,
                                        center.x * inv, center.y * inv);
        const f32 n1 = terrain::noise01(params.seed ^ 0x1b28u,
                                        center.x * inv, center.y * inv);
        const f32 n2 = terrain::noise01(params.seed ^ 0x2c39u,
                                        center.x * inv, center.y * inv);
        const f32 n3 = terrain::noise01(params.seed ^ 0x3d4au,
                                        center.x * inv, center.y * inv);
        const f32 n4 = terrain::noise01(params.seed ^ 0x4e5bu,
                                        center.x * inv, center.y * inv);
        // Moss claims the shaded wet forest floor: the wetter and deeper
        // in the woods, the more clumps flip to it — exactly where blade
        // grass thins out.
        const f32 wetBias =
            glm::clamp(0.5f - 0.35f * fields.temperature +
                           0.5f * fields.biomeWetness,
                       0.0f, 1.0f);
        const f32 forest = forestAt(params, center.x, center.y);
        f32 scores[kGrassSpeciesCount] = {
            0.25f + 0.9f * n0,
            (0.25f + 0.95f * dryBias) * n1,
            0.6f * n2 * (0.4f + 0.6f * presence),
            (n3 > 0.78f ? 0.85f : 0.04f) * (1.0f - 0.6f * dryBias),
            (0.10f + 1.30f * wetBias * forest) * (0.4f + 0.6f * n4),
            0.0f, // lichen grows through the rock-crevice path only
        };
        const u32 zoneVariant = glm::min<u32>(
            terrain::grassZoneAt(center.x, center.y).variantA,
            kGrassVariantCount - 1);
        for (u32 s = 0; s < kGrassSpeciesCount; ++s) {
            scores[s] *= kZoneSpeciesBias[zoneVariant][s];
        }
        Clump info;
        for (u32 s = 1; s < kGrassSpeciesCount; ++s) {
            if (scores[s] > scores[info.species]) {
                info.species = static_cast<u8>(s);
            }
        }
        info.shade = 0.86f + 0.28f * static_cast<f32>(hashU32(
                                         static_cast<u32>(key) ^
                                         static_cast<u32>(key >> 32))) *
                                         (1.0f / 4294967295.0f);
        info.center = center;
        return info;
    }

    const TerrainParams& params;
    std::unordered_map<u64, Clump> clumps;
};

// What one cell's candidates share, resolved once per cell.
struct CellGround {
    f32 presence { 0.0f };  // patch presence: rim 0 -> solid volume 1
    f32 acceptP { 0.0f };   // per-candidate acceptance probability
    f32 dwarf { 1.0f };     // blade shrink on the grass/rock fringe
    bool crevice { false }; // rock crevices: dry tufts + lichen, no clump
    Vec3 normal { 0.0f, 1.0f, 0.0f };
    terrain::RegionFields fields;
    CornerLattice::Corners corners {};
};

// Resolves one cell's shared ground, or nothing when the cell grows no
// blade (outside a patch, under local water, solid rock without
// crevices).
std::optional<CellGround> sampleCell(const TerrainParams& params,
                                     const GrassScatterTuning& tuning,
                                     const ChunkFrame& frame,
                                     const CornerLattice& lattice, u32 cgx,
                                     u32 cgz) {
    const f32 cellX = frame.cellCenterX(cgx);
    const f32 cellZ = frame.cellCenterZ(cgz);
    // Near-BINARY presence: even moderately inside the mask the clump is
    // at FULL density (the solid volume); only the rim thins, fast, so
    // patches keep their silhouette.
    const f32 patch = patchMask(params.seed, tuning, cellX, cellZ);
    const f32 presence =
        glm::smoothstep(tuning.presenceLo, tuning.presenceHi, patch);
    if (presence < 0.02f) {
        return std::nullopt;
    }
    CellGround cell;
    cell.presence = presence;
    cell.corners = lattice.corners(cgx, cgz);
    const CornerLattice::Corners& c = cell.corners;
    // Cell normal from its corners — grass shading barely perturbs it,
    // lattice precision is plenty.
    cell.normal = glm::normalize(
        Vec3 { -(c.h10 - c.h00 + c.h11 - c.h01) / (2.0f * frame.cellSize),
               1.0f,
               -(c.h01 - c.h00 + c.h11 - c.h10) / (2.0f * frame.cellSize) });
    const f32 hMid = 0.25f * (c.h00 + c.h10 + c.h01 + c.h11);
    // Density RAMP instead of the historical boolean cutoff: the splat's
    // grass/rock blend zone is ALSO the blade rarefaction zone
    // (docs/PAYSAGE.md §1.7 — AAA transitions are never binary).
    // materialCutoff keeps its meaning as the full-density threshold;
    // blades thin AND shrink below it. Where the ramp dies on solid
    // rock, sparse dwarf DRY tufts grow in the crevices instead (the
    // cross-scatter pattern).
    // One region-fields sample serves the weights AND the species scores
    // (5 biomeBlended per eval — the expensive half of a cell).
    cell.fields = terrain::regionFieldsAt(params, cellX, cellZ);
    const terrain::MaterialWeights weights = terrain::materialWeightsAt(
        params, cellX, cellZ, hMid, cell.normal, cell.fields);
    if (terrain::underLocalWater(params, frame.water, cellX, cellZ, hMid,
                                 0.1f)) {
        return std::nullopt;
    }
    const f32 matRamp = glm::smoothstep(tuning.materialCutoff - 0.37f,
                                        tuning.materialCutoff, weights.grass);
    cell.acceptP = presence * matRamp;
    cell.dwarf = 0.55f + 0.45f * matRamp;
    if (matRamp < 0.02f) {
        const f32 rocky = weights.rock + weights.cliff;
        if (rocky < 0.45f) {
            return std::nullopt;
        }
        cell.crevice = true;
        cell.acceptP = 0.05f * rocky;
        cell.dwarf = 0.45f;
    }
    if (cell.acceptP < 0.01f) {
        return std::nullopt;
    }
    return cell;
}

// One cell's candidate loop: jittered position, acceptance, species,
// shape and the instance fill. Each candidate owns a HashRng seeded from
// the chunk and its index on the fine blade grid, so its draws never
// depend on what grew around it; the draw ORDER (position, acceptance,
// species, scale, overshoot, lean, yaw, flutter, tint jitter) is part of
// the scatter's determinism contract.
void scatterCell(const TerrainParams& params, const ChunkFrame& frame,
                 const CellGround& cell, ClumpPicker& clumps, u32 cgx,
                 u32 cgz, vector<GrassSystem::Instance>& out) {
    const CornerLattice::Corners& c = cell.corners;
    const f32 cellMinX = frame.cellMinX(cgx);
    const f32 cellMinZ = frame.cellMinZ(cgz);
    for (u32 sz = 0; sz < ChunkFrame::kCell; ++sz) {
        for (u32 sx = 0; sx < ChunkFrame::kCell; ++sx) {
            const u32 gx = cgx * ChunkFrame::kCell + sx;
            const u32 gz = cgz * ChunkFrame::kCell + sz;
            if (gx >= frame.perSide || gz >= frame.perSide) {
                continue;
            }
            HashRng rng { hashU32(params.seed ^ 0x6b79a3f1u) ^
                          hashU32(static_cast<u32>(frame.cx * 73856093 ^
                                                   frame.cz * 19349663) ^
                                  (gz * frame.perSide + gx)) };
            const f32 x = frame.originX +
                          (static_cast<f32>(gx) + rng.next()) * frame.spacing;
            const f32 z = frame.originZ +
                          (static_cast<f32>(gz) + rng.next()) * frame.spacing;
            if (rng.next() >= cell.acceptP) {
                continue;
            }
            const f32 fx = (x - cellMinX) / frame.cellSize;
            const f32 fz = (z - cellMinZ) / frame.cellSize;
            const f32 h = glm::mix(glm::mix(c.h00, c.h10, fx),
                                   glm::mix(c.h01, c.h11, fx), fz);
            const Vec3 rootAlbedo = glm::mix(glm::mix(c.a00, c.a10, fx),
                                             glm::mix(c.a01, c.a11, fx), fz);
            const ClumpPicker::Clump& clump =
                clumps.pick(x, z, cell.fields, cell.presence);
            // Crevice growth on rock: dry tufts and lichen plates share
            // the cracks.
            const u32 species =
                cell.crevice ? (rng.next() < 0.45f ? GrassSpecies_Lichen
                                                   : GrassSpecies_Dry)
                             : clump.species;
            // Height: near-uniform inside the volume so the top reads as
            // one surface; the rim droops shorter, and ~12% of blades
            // overshoot — the tips poking above the mass (the BotW
            // tell). Species height applies here (the shader table's x
            // lane is scatter-side); `dwarf` shrinks the grass/rock
            // fringe blades so the texture blend zone and the blade
            // shrink coincide.
            f32 scale = (0.70f + rng.next() * 0.30f) *
                        (0.55f + 0.45f * cell.presence) * cell.dwarf *
                        kGrassSpeciesShape[species][0];
            if (rng.next() < 0.12f) {
                scale *= 1.35f;
            }
            // Rim blades lean/curve harder — the clump spills over its
            // sides instead of ending in a wall.
            const f32 lean = glm::min(
                1.0f, rng.next() + (1.0f - cell.presence) * 0.6f);
            // Blades fan AWAY from their clump center (GoT): the yaw
            // mixes outward with jitter — a clump reads as one tuft, not
            // parallel needles.
            const f32 yaw =
                std::atan2(x - clump.center.x, z - clump.center.y) +
                (rng.next() - 0.5f) * 2.6f;
            out.push_back({
                .positionScale = { x, h, z, scale },
                .params = { yaw,
                            rng.next() * core::kTau, // flutter
                            rng.next(),              // tint jitter
                            lean },                  // lean amount
                // BotW shading + the root's ground color.
                .groundNormal = { cell.normal, packAlbedo(rootAlbedo) },
                .species = { static_cast<f32>(species), clump.shade, 0.0f,
                             0.0f },
            });
        }
    }
}

} // namespace

vector<GrassSystem::Instance> scatterGrass(const TerrainParams& params,
                                           const GrassScatterTuning& tuning,
                                           i32 cx, i32 cz) {
    const ChunkFrame frame = ChunkFrame::build(params, tuning, cx, cz);
    const CornerLattice lattice = CornerLattice::build(params, tuning, frame);
    ClumpPicker clumps { params };

    vector<GrassSystem::Instance> result;
    result.reserve(frame.perSide * frame.perSide / 4);
    for (u32 cgz = 0; cgz < frame.cells; ++cgz) {
        for (u32 cgx = 0; cgx < frame.cells; ++cgx) {
            const std::optional<CellGround> cell =
                sampleCell(params, tuning, frame, lattice, cgx, cgz);
            if (!cell) {
                continue;
            }
            scatterCell(params, frame, *cell, clumps, cgx, cgz, result);
        }
    }
    // Sort by the density-LOD keep key (flutter phase — uniform random
    // per blade): grass.vert clips blades whose key exceeds the metric
    // density curve, so a PREFIX of this buffer is EXACTLY the set the
    // shader keeps. draw() cuts the prefix with the same curve — the
    // vertex shader never even sees the tail it would have clipped.
    std::sort(result.begin(), result.end(),
              [](const GrassSystem::Instance& a,
                 const GrassSystem::Instance& b) {
                  return a.params.y < b.params.y;
              });
    return result;
}

void GrassSystem::create(rhi::Device& device, ShaderLibrary& shaders,
                         core::JobSystem& jobSystem) {
    streamer.create(jobSystem, "grassScatter");

    bladeVertexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex, .size = sizeof(kBladeVertices) },
        kBladeVertices) };
    bladeIndexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Index, .size = sizeof(kBladeIndices) },
        kBladeIndices) };
    bladeIndexCount = static_cast<u32>(std::size(kBladeIndices));

    shaders.load(kGrassShader, { render::kFrameUbo },
                 { { "uShadowMap", 1 }, { "uFireScorch", 10 } });
    refreshPipeline(device, shaders);
}

void GrassSystem::destroy(rhi::Device& device) {
    (void)device; // Unique handles free through their device
    streamer.invalidateAll([](Chunk&) {});
    instances = 0;
    pipeline.reset();
    bladeIndexBuffer.reset();
    bladeVertexBuffer.reset();
}

void GrassSystem::regenerate(rhi::Device& device) {
    (void)device; // the erases free the instance buffers
    streamer.invalidateAll([](Chunk&) {});
    instances = 0;
}

void GrassSystem::invalidateChunks(rhi::Device& /*device*/,
                                   const vector<u64>& keys) {
    for (const u64 key : keys) {
        const auto it = streamer.chunks.find(key);
        if (it == streamer.chunks.end()) {
            continue;
        }
        if (!it->second.resident) {
            // Build in flight against the OLD terrain: the pump drops
            // the landing payload and the ring re-requests.
            it->second.stale = true;
            continue;
        }
        instances -= it->second.instanceCount;
        // update() re-requests + re-scatters with new heights (the erase
        // frees the instance buffer).
        streamer.chunks.erase(it);
    }
}

void GrassSystem::update(rhi::Device& device, const TerrainParams& params,
                         const Vec3& cameraPos, bool holdRequests) {
    frameIndices = 0; // the frame's draw() sums into these
    frameBlades = 0;
    // Budgeted uploads (the ring mechanics live in core::ChunkStreamer;
    // this lambda is the grass-specific accept + GPU upload).
    streamer.pump(kMaxUploadsPerFrame, 0.0, [&](u64 key, auto& built) {
        const auto it = streamer.chunks.find(key);
        if (it == streamer.chunks.end() || it->second.resident) {
            return false;
        }
        if (it->second.stale) {
            // Scattered against terrain/water that changed mid-flight:
            // drop the payload, the ring re-requests with fresh params.
            streamer.chunks.erase(it);
            return false;
        }
        Chunk& chunk = it->second;
        chunk.instanceCount = static_cast<u32>(built.payload.size());
        if (chunk.instanceCount > 0) {
            chunk.instanceBuffer = { device, device.createBuffer(
                { .usage = rhi::BufferUsage::Vertex,
                  .size = built.payload.size() * sizeof(Instance) },
                built.payload.data()) };
            chunk.minY = built.payload[0].positionScale.y;
            chunk.maxY = chunk.minY;
            for (const Instance& instance : built.payload) {
                chunk.minY = glm::min(chunk.minY, instance.positionScale.y);
                chunk.maxY = glm::max(chunk.maxY, instance.positionScale.y);
            }
        }
        chunk.resident = true;
        instances += chunk.instanceCount;
        return true;
    });

    // Request missing chunks in the grass ring — nearest first, budgeted
    // (the rest is re-detected next frame; the state IS the queue).
    const i32 camCx = core::chunkCoordOf(cameraPos.x, TerrainSystem::kChunkSize);
    const i32 camCz = core::chunkCoordOf(cameraPos.z, TerrainSystem::kChunkSize);
    if (!holdRequests) {
        streamer.requestMissing(
            camCx, camCz, kViewRadius, kMaxRequestsPerFrame,
            [&](i32 cx, i32 cz, i32, i32) {
                return !streamer.chunks.contains(core::chunkKey(cx, cz));
            },
            [&](i32 cx, i32 cz, i32, i32) {
                streamer.chunks.emplace(core::chunkKey(cx, cz), Chunk {});
                streamer.enqueueBuild(
                    cx, cz, [params, tuning = scatterTuning, cx, cz] {
                        return scatterGrass(params, tuning, cx, cz);
                    });
            });
    }

    // Evict beyond hysteresis.
    streamer.evictFar(camCx, camCz, kEvictRadius, [&](Chunk& chunk) {
        // The erase frees the instance buffer.
        if (chunk.resident) {
            instances -= chunk.instanceCount;
        }
    });
}

void GrassSystem::refreshPipeline(rhi::Device& device,
                                  ShaderLibrary& shaders) {
    pipeline.ensure(device, shaders, [&] {
        return device.createPipeline(
            { .shader = shaders.get(kGrassShader),
              .vertexBuffers =
                  { { .stride = 2 * sizeof(f32),
                      .attributes = { { .location = 0,
                                        .format = rhi::VertexFormat::F32x2,
                                        .offset = 0 } } },
                    { .stride = sizeof(Instance),
                      .stepMode = rhi::VertexStepMode::Instance,
                      .attributes =
                          { { .location = 2,
                              .format = rhi::VertexFormat::F32x4,
                              .offset = offsetof(Instance, positionScale) },
                            { .location = 3,
                              .format = rhi::VertexFormat::F32x4,
                              .offset = offsetof(Instance, params) },
                            { .location = 4,
                              .format = rhi::VertexFormat::F32x4,
                              .offset = offsetof(Instance, groundNormal) },
                            { .location = 1,
                              .format = rhi::VertexFormat::F32x4,
                              .offset = offsetof(Instance, species) } } } },
              // Pure geometry, opaque: depth-write on so blades sort
              // against the terrain and each other; visible from both
              // sides.
              .depth = render::kDepthOpaque,
              .cull = rhi::CullMode::None });
    });
}

void GrassSystem::draw(rhi::CommandBuffer& cmd,
                       rhi::BindGroupHandle frameBindGroup,
                       rhi::BindGroupHandle shadowBindGroup,
                       const Vec3& cameraPos, const Frustum* frustum) {
    cmd.setPipeline(pipeline);
    cmd.setBindGroup(0, frameBindGroup);
    if (shadowBindGroup.id != 0) {
        cmd.setBindGroup(2, shadowBindGroup);
    }
    cmd.setVertexBuffer(0, bladeVertexBuffer);
    cmd.setIndexBuffer(bladeIndexBuffer, rhi::IndexFormat::U16);
    struct Draw {
        f32 nearest;
        const Chunk* chunk;
        u32 count;
    };
    vector<Draw> draws;
    draws.reserve(streamer.chunks.size());
    for (const auto& [key, chunk] : streamer.chunks) {
        if (!chunk.resident || chunk.instanceCount == 0) {
            continue;
        }
        const f32 minX =
            static_cast<f32>(core::chunkKeyCx(key)) * TerrainSystem::kChunkSize;
        const f32 minZ =
            static_cast<f32>(core::chunkKeyCz(key)) * TerrainSystem::kChunkSize;
        const f32 dx = glm::max(
            glm::max(minX - cameraPos.x,
                     cameraPos.x - (minX + TerrainSystem::kChunkSize)),
            0.0f);
        const f32 dz = glm::max(
            glm::max(minZ - cameraPos.z,
                     cameraPos.z - (minZ + TerrainSystem::kChunkSize)),
            0.0f);
        const f32 nearest = std::sqrt(dx * dx + dz * dz);
        if (nearest > renderTuning.fadeEnd) {
            continue; // every blade fully sunk into the ground
        }
        if (frustum) {
            // Headroom over the blade roots (height × scale + wind).
            if (!frustum->intersectsAabb(
                    { minX, chunk.minY - 0.5f, minZ },
                    { minX + TerrainSystem::kChunkSize,
                      chunk.maxY + renderTuning.bladeHeight + 0.8f,
                      minZ + TerrainSystem::kChunkSize })) {
                continue;
            }
        }
        // Density LOD prefix: instances are SORTED by the keep key, so
        // drawing the first N is exactly the set grass.vert keeps at
        // this chunk's NEAREST distance (density is monotonic in dist —
        // every blade in the chunk is at least that far). Same curve as
        // the shader (both read the SAME tuning — grass.vert through
        // uGrassLodInfo), +3% margin for the key sampling noise.
        const f32 thin = glm::smoothstep(renderTuning.thinStart,
                                         renderTuning.thinEnd, nearest);
        const f32 density = glm::min(
            1.0f, glm::mix(1.0f, renderTuning.farDensity, thin) + 0.03f);
        const u32 count = glm::min(
            chunk.instanceCount,
            1u + static_cast<u32>(
                     static_cast<f32>(chunk.instanceCount) * density));
        draws.push_back({ nearest, &chunk, count });
    }
    // Front-to-back: near blades fill the depth buffer first, so the
    // grass behind them early-z-rejects instead of shading over.
    std::sort(draws.begin(), draws.end(),
              [](const Draw& a, const Draw& b) {
                  return a.nearest < b.nearest;
              });
    for (const Draw& d : draws) {
        cmd.setVertexBuffer(1, d.chunk->instanceBuffer);
        cmd.drawIndexed(bladeIndexCount, d.count);
        frameIndices += bladeIndexCount * d.count;
        frameBlades += d.count;
    }
}

} // namespace render
