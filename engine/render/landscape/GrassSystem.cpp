#include "engine/render/landscape/GrassSystem.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <unordered_map>

#include "engine/core/Hash.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/render/landscape/GrassSpecies.hpp"
#include "engine/render/landscape/SplatTextures.hpp"
#include "engine/render/landscape/VegetationSystem.hpp" // forestMask
#include "engine/render/landscape/TerrainSystem.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

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

} // namespace

vector<GrassSystem::Instance> scatterGrass(const TerrainParams& params,
                                           const GrassScatterTuning& tuning,
                                           i32 cx, i32 cz) {
    const f32 originX = static_cast<f32>(cx) * TerrainSystem::kChunkSize;
    const f32 originZ = static_cast<f32>(cz) * TerrainSystem::kChunkSize;
    const f32 spacing = glm::max(0.05f, tuning.spacing);
    const u32 perSide =
        static_cast<u32>(TerrainSystem::kChunkSize / spacing);

    // CELL-MAJOR scatter (per-candidate noise evals on the fine blade
    // grid would saturate every worker for seconds at boot).
    // The masks vary over METERS, not centimeters — patch,
    // material and normal are evaluated once per cell, and the terrain
    // height on the cell-corner lattice (bilinear per blade — at or
    // below the render mesh's own sampling error). Bare cells cost two
    // noise evals total; only the cheap per-blade jitter runs per
    // candidate.
    constexpr u32 kCell = 4; // candidates per cell side (0.6 m cells)
    const u32 cells = (perSide + kCell - 1) / kCell;
    const f32 cellSize = spacing * static_cast<f32>(kCell);

    vector<f32> cornerH((cells + 1) * (cells + 1));
    // Ground albedo on the same corner lattice (bilinear per blade): the
    // blotch field varies over meters, and per-candidate FBM evals are
    // exactly what the cell-major layout exists to avoid. Each corner
    // wraps its own uv — the tile is seamless, so interpolating the
    // COLORS across a tile boundary stays continuous.
    vector<Vec3> cornerAlbedo((cells + 1) * (cells + 1));
    for (u32 gz = 0; gz <= cells; ++gz) {
        for (u32 gx = 0; gx <= cells; ++gx) {
            const f32 wx = originX + static_cast<f32>(gx) * cellSize;
            const f32 wz = originZ + static_cast<f32>(gz) * cellSize;
            cornerH[gz * (cells + 1) + gx] =
                terrain::height(params, wx, wz);
            const f32 su = wx * tuning.splatUvScale;
            const f32 sv = wz * tuning.splatUvScale;
            // Macro tint applies to the root color exactly as the terrain
            // shader applies it to the ground (same regionShadingAt, same
            // strength lerp) — meadow and terrain keep ONE color source.
            // The base color is the ACTIVE set's mean for the corner's
            // ground variant (grass zones), so the raccord holds on the
            // cooked set and across zone borders alike.
            const Vec3 tint =
                glm::mix(Vec3 { 1.0f },
                         terrain::regionShadingAt(params, wx, wz).tint,
                         tuning.tintStrength);
            const Vec3 base =
                tuning.rootAlbedoBase[terrain::grassZoneAt(wx, wz)
                                          .variantA];
            cornerAlbedo[gz * (cells + 1) + gx] =
                base *
                grassBlotch(su - std::floor(su), sv - std::floor(sv)) *
                tint;
        }
    }

    // Species per Voronoi clump (jittered grid, ~2.4 m): neighboring
    // blades share species/shade and fan away from their clump center —
    // patches read as vegetation, not per-blade noise (the GoT model,
    // docs/GRASS-REDO.md). Sites are resolved per blade (nearest of the
    // 3x3 neighbor sites — pure arithmetic) and their pick is cached per
    // chunk build; per-species low-frequency noises make one species win
    // over several clumps at a time.
    constexpr f32 kClumpSize = 2.4f;
    struct ClumpInfo {
        u8 species { 0 };
        f32 shade { 1.0f };
        Vec2 center {};
    };
    std::unordered_map<u64, ClumpInfo> clumps;
    const auto clumpSite = [&](i32 gx, i32 gz) {
        const u32 h = hashU32(params.seed ^ 0x51c7a3b9u ^
                              hashU32(static_cast<u32>(gx) * 0x9e3779b9u ^
                                      static_cast<u32>(gz) * 0x85ebca6bu));
        const f32 jx = static_cast<f32>(h & 0xffffu) * (1.0f / 65535.0f);
        const f32 jz = static_cast<f32>(h >> 16) * (1.0f / 65535.0f);
        return Vec2 { (static_cast<f32>(gx) + 0.15f + 0.7f * jx) *
                          kClumpSize,
                      (static_cast<f32>(gz) + 0.15f + 0.7f * jz) *
                          kClumpSize };
    };
    const auto clumpAt = [&](f32 x, f32 z,
                             const terrain::RegionFields& fields,
                             f32 presence) -> const ClumpInfo& {
        const i32 bx = static_cast<i32>(std::floor(x / kClumpSize));
        const i32 bz = static_cast<i32>(std::floor(z / kClumpSize));
        i32 bestX = bx;
        i32 bestZ = bz;
        Vec2 bestSite {};
        f32 bestD = 1e9f;
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                const Vec2 site = clumpSite(bx + dx, bz + dz);
                const f32 d = (site.x - x) * (site.x - x) +
                              (site.y - z) * (site.y - z);
                if (d < bestD) {
                    bestD = d;
                    bestX = bx + dx;
                    bestZ = bz + dz;
                    bestSite = site;
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
        // Species scores at the winning site. Aridity mirrors the macro
        // tint's climate read (temperature/biomeWetness), so dry straw
        // grows where the ground reads parched.
        const f32 inv = 1.0f / 9.0f; // ~9 m species patches
        const f32 dryBias =
            glm::clamp(0.5f + 0.35f * fields.temperature -
                           0.4f * fields.biomeWetness,
                       0.0f, 1.0f);
        const f32 n0 = terrain::noise01(params.seed ^ 0x0a17u,
                                        bestSite.x * inv, bestSite.y * inv);
        const f32 n1 = terrain::noise01(params.seed ^ 0x1b28u,
                                        bestSite.x * inv, bestSite.y * inv);
        const f32 n2 = terrain::noise01(params.seed ^ 0x2c39u,
                                        bestSite.x * inv, bestSite.y * inv);
        const f32 n3 = terrain::noise01(params.seed ^ 0x3d4au,
                                        bestSite.x * inv, bestSite.y * inv);
        const f32 n4 = terrain::noise01(params.seed ^ 0x4e5bu,
                                        bestSite.x * inv, bestSite.y * inv);
        // Moss claims the shaded wet forest floor (P4 micro tier): the
        // wetter and deeper in the woods, the more clumps flip to it —
        // exactly where blade grass thins out.
        const f32 wetBias =
            glm::clamp(0.5f - 0.35f * fields.temperature +
                           0.5f * fields.biomeWetness,
                       0.0f, 1.0f);
        const f32 forest = forestMask(params.seed, bestSite.x, bestSite.y);
        f32 scores[kGrassSpeciesCount] = {
            0.25f + 0.9f * n0,
            (0.25f + 0.95f * dryBias) * n1,
            0.6f * n2 * (0.4f + 0.6f * presence),
            (n3 > 0.78f ? 0.85f : 0.04f) * (1.0f - 0.6f * dryBias),
            (0.10f + 1.30f * wetBias * forest) * (0.4f + 0.6f * n4),
            0.0f, // lichen grows through the rock-crevice path only
        };
        // Ground-variant bias (terrain_zones — v1 worn path, v2 stones,
        // v3 dirt): what the ground texture shows and what grows on it
        // agree — wear and stones thin the meadow, dirt turns strawy.
        switch (terrain::grassZoneAt(bestSite.x, bestSite.y).variantA) {
        case 1:
            scores[GrassSpecies_Meadow] *= 0.6f;
            scores[GrassSpecies_Oat] *= 0.8f;
            scores[GrassSpecies_Flower] *= 0.7f;
            scores[GrassSpecies_Moss] *= 1.5f; // worn ground: moss plates
            break;
        case 2:
            scores[GrassSpecies_Meadow] *= 0.75f;
            scores[GrassSpecies_Dry] *= 1.3f;
            break;
        case 3:
            scores[GrassSpecies_Dry] *= 1.5f;
            scores[GrassSpecies_Meadow] *= 0.55f;
            scores[GrassSpecies_Moss] *= 1.4f;
            break;
        default:
            scores[GrassSpecies_Meadow] *= 1.25f;
            break;
        }
        ClumpInfo info;
        for (u32 s = 1; s < kGrassSpeciesCount; ++s) {
            if (scores[s] > scores[info.species]) {
                info.species = static_cast<u8>(s);
            }
        }
        info.shade = 0.86f + 0.28f * static_cast<f32>(hashU32(
                                         static_cast<u32>(key) ^
                                         static_cast<u32>(key >> 32))) *
                                         (1.0f / 4294967295.0f);
        info.center = bestSite;
        return clumps.emplace(key, info).first->second;
    };

    vector<GrassSystem::Instance> result;
    result.reserve(perSide * perSide / 4);
    for (u32 cgz = 0; cgz < cells; ++cgz) {
        for (u32 cgx = 0; cgx < cells; ++cgx) {
            // Near-BINARY presence: even moderately inside
            // the mask the clump is at FULL density (the solid volume);
            // only the rim thins, fast, so patches keep their silhouette.
            const f32 patch = patchMask(
                params.seed, tuning,
                originX + (static_cast<f32>(cgx) + 0.5f) * cellSize,
                originZ + (static_cast<f32>(cgz) + 0.5f) * cellSize);
            const f32 presence = glm::smoothstep(tuning.presenceLo,
                                                 tuning.presenceHi, patch);
            if (presence < 0.02f) {
                continue;
            }
            const f32 h00 = cornerH[cgz * (cells + 1) + cgx];
            const f32 h10 = cornerH[cgz * (cells + 1) + cgx + 1];
            const f32 h01 = cornerH[(cgz + 1) * (cells + 1) + cgx];
            const f32 h11 = cornerH[(cgz + 1) * (cells + 1) + cgx + 1];
            const Vec3& a00 = cornerAlbedo[cgz * (cells + 1) + cgx];
            const Vec3& a10 = cornerAlbedo[cgz * (cells + 1) + cgx + 1];
            const Vec3& a01 = cornerAlbedo[(cgz + 1) * (cells + 1) + cgx];
            const Vec3& a11 =
                cornerAlbedo[(cgz + 1) * (cells + 1) + cgx + 1];
            // Cell normal from its corners — grass shading barely
            // perturbs it, lattice precision is plenty.
            const Vec3 n = glm::normalize(
                Vec3 { -(h10 - h00 + h11 - h01) / (2.0f * cellSize), 1.0f,
                       -(h01 - h00 + h11 - h10) / (2.0f * cellSize) });
            const f32 hMid = 0.25f * (h00 + h10 + h01 + h11);
            // Density RAMP instead of the historical boolean cutoff: the
            // splat's grass/rock blend zone is ALSO the blade rarefaction
            // zone (docs/GRASS-REDO.md — AAA transitions are never
            // binary). materialCutoff keeps its meaning as the
            // full-density threshold; blades thin AND shrink below it.
            // Where the ramp dies on solid rock, sparse dwarf DRY tufts
            // grow in the crevices instead (the cross-scatter pattern).
            const f32 cellX =
                originX + (static_cast<f32>(cgx) + 0.5f) * cellSize;
            const f32 cellZ =
                originZ + (static_cast<f32>(cgz) + 0.5f) * cellSize;
            const terrain::MaterialWeights cellWeights =
                terrain::materialWeightsAt(params, cellX, cellZ, hMid, n);
            if (terrain::underLocalWater(params, cellX, cellZ, hMid,
                                         0.1f)) {
                continue;
            }
            const f32 matRamp =
                glm::smoothstep(tuning.materialCutoff - 0.37f,
                                tuning.materialCutoff, cellWeights.grass);
            f32 acceptP = presence * matRamp;
            f32 dwarf = 0.55f + 0.45f * matRamp;
            bool crevice = false;
            if (matRamp < 0.02f) {
                const f32 rocky = cellWeights.rock + cellWeights.cliff;
                if (rocky < 0.45f) {
                    continue;
                }
                crevice = true;
                acceptP = 0.05f * rocky;
                dwarf = 0.45f;
            }
            if (acceptP < 0.01f) {
                continue;
            }
            // Climate fields for the species scores (varies over ~16 m —
            // one eval per cell is plenty).
            const terrain::RegionFields cellFields =
                terrain::regionFieldsAt(params, cellX, cellZ);
            for (u32 sz = 0; sz < kCell; ++sz) {
                for (u32 sx = 0; sx < kCell; ++sx) {
                    const u32 gx = cgx * kCell + sx;
                    const u32 gz = cgz * kCell + sz;
                    if (gx >= perSide || gz >= perSide) {
                        continue;
                    }
                    HashRng rng { hashU32(params.seed ^ 0x6b79a3f1u) ^
                                  hashU32(static_cast<u32>(cx * 73856093 ^
                                                           cz * 19349663) ^
                                          (gz * perSide + gx)) };
                    const f32 x = originX +
                                  (static_cast<f32>(gx) + rng.next()) *
                                      spacing;
                    const f32 z = originZ +
                                  (static_cast<f32>(gz) + rng.next()) *
                                      spacing;
                    if (rng.next() >= acceptP) {
                        continue;
                    }
                    const f32 fx =
                        (x - (originX + static_cast<f32>(cgx) * cellSize)) /
                        cellSize;
                    const f32 fz =
                        (z - (originZ + static_cast<f32>(cgz) * cellSize)) /
                        cellSize;
                    const f32 h = glm::mix(glm::mix(h00, h10, fx),
                                           glm::mix(h01, h11, fx), fz);
                    const Vec3 rootAlbedo =
                        glm::mix(glm::mix(a00, a10, fx),
                                 glm::mix(a01, a11, fx), fz);
                    const ClumpInfo& clump =
                        clumpAt(x, z, cellFields, presence);
                    // Crevice growth on rock: dry tufts and lichen
                    // plates share the cracks (P4).
                    const u32 species =
                        crevice ? (rng.next() < 0.45f
                                       ? GrassSpecies_Lichen
                                       : GrassSpecies_Dry)
                                : clump.species;
                    // Height: near-uniform inside the volume so the top
                    // reads as one surface; the rim droops shorter, and
                    // ~12% of blades overshoot — the tips poking above
                    // the mass (the BotW tell). Species height applies
                    // here (the shader table's x lane is scatter-side);
                    // `dwarf` shrinks the grass/rock fringe blades so the
                    // texture blend zone and the blade shrink coincide.
                    f32 scale = (0.70f + rng.next() * 0.30f) *
                                (0.55f + 0.45f * presence) * dwarf *
                                kGrassSpeciesShape[species][0];
                    if (rng.next() < 0.12f) {
                        scale *= 1.35f;
                    }
                    // Rim blades lean/curve harder — the clump spills
                    // over its sides instead of ending in a wall.
                    const f32 lean = glm::min(
                        1.0f, rng.next() + (1.0f - presence) * 0.6f);
                    // Blades fan AWAY from their clump center (GoT):
                    // the yaw mixes outward with jitter — a clump reads
                    // as one tuft, not parallel needles.
                    const f32 yaw =
                        std::atan2(x - clump.center.x, z - clump.center.y) +
                        (rng.next() - 0.5f) * 2.6f;
                    result.push_back({
                        .positionScale = { x, h, z, scale },
                        .params = { yaw,
                                    rng.next() * 6.2831853f, // flutter
                                    rng.next(),              // tint jitter
                                    lean },                  // lean amount
                        // BotW shading + the root's ground color.
                        .groundNormal = { n, packAlbedo(rootAlbedo) },
                        .species = { static_cast<f32>(species),
                                     clump.shade, 0.0f, 0.0f },
                    });
                }
            }
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
    streamer.create(jobSystem);

    bladeVertexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex, .size = sizeof(kBladeVertices) },
        kBladeVertices) };
    bladeIndexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Index, .size = sizeof(kBladeIndices) },
        kBladeIndices) };
    bladeIndexCount = static_cast<u32>(std::size(kBladeIndices));

    shaders.load(kGrassShader, { { "FrameUbo", 0 } },
                 { { "uShadowMap", 1 } });
    buildPipeline(device, shaders);
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
    // Budgeted uploads (the ring mechanics live in ChunkStreamer;
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
    const i32 camCx = chunkCoordOf(cameraPos.x, TerrainSystem::kChunkSize);
    const i32 camCz = chunkCoordOf(cameraPos.z, TerrainSystem::kChunkSize);
    if (!holdRequests) {
        streamer.requestMissing(
            camCx, camCz, kViewRadius, kMaxRequestsPerFrame,
            [&](i32 cx, i32 cz, i32, i32) {
                return !streamer.chunks.contains(chunkKey(cx, cz));
            },
            [&](i32 cx, i32 cz, i32, i32) {
                streamer.chunks.emplace(chunkKey(cx, cz), Chunk {});
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

void GrassSystem::buildPipeline(rhi::Device& device, ShaderLibrary& shaders) {
    pipeline = { device, device.createPipeline( // frees the old one
        { .shader = shaders.get(kGrassShader),
          .vertexBuffers =
              { { .stride = 2 * sizeof(f32),
                  .attributes = { { .location = 0,
                                    .format = rhi::VertexFormat::F32x2,
                                    .offset = 0 } } },
                { .stride = sizeof(Instance),
                  .stepMode = rhi::VertexStepMode::Instance,
                  .attributes = { { .location = 2,
                                    .format = rhi::VertexFormat::F32x4,
                                    .offset = offsetof(Instance,
                                                       positionScale) },
                                  { .location = 3,
                                    .format = rhi::VertexFormat::F32x4,
                                    .offset = offsetof(Instance, params) },
                                  { .location = 4,
                                    .format = rhi::VertexFormat::F32x4,
                                    .offset = offsetof(
                                        Instance, groundNormal) },
                                  { .location = 1,
                                    .format = rhi::VertexFormat::F32x4,
                                    .offset = offsetof(Instance,
                                                       species) } } } },
          // Pure geometry, opaque: depth-write on so blades sort against the
          // terrain and each other; visible from both sides.
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Greater }, // reversed-Z
          .cull = rhi::CullMode::None }) };
    shaderGeneration = shaders.generation(kGrassShader);
}

void GrassSystem::refreshPipeline(rhi::Device& device,
                                  ShaderLibrary& shaders) {
    if (shaders.generation(kGrassShader) != shaderGeneration) {
        buildPipeline(device, shaders);
    }
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
            static_cast<f32>(chunkKeyCx(key)) * TerrainSystem::kChunkSize;
        const f32 minZ =
            static_cast<f32>(chunkKeyCz(key)) * TerrainSystem::kChunkSize;
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
