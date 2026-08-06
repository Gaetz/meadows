#include "engine/render/landscape/VegetationSystem.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/Hash.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/render/MeshVertexLayout.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/render/landscape/TerrainSystem.hpp"
#include "engine/assets/VertexAoCache.hpp"
#include "engine/platform/Paths.hpp"
#include "engine/render/landscape/TreeGenerator.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {

constexpr const char* kTreeShader = "tree";
constexpr const char* kPropCasterShader = "shadow_prop";
// Realistic-scale trees: x8 height against the player,
// so 2x the candidate spacing = 1/4 the density — giant forests, not
// hedges of them.
constexpr f32 kTreeSpacing = 8.0f; // meters between scatter candidates

// hashU32 / HashRng now live in engine/core/Hash.hpp (shared scatter hash family).
using core::hashU32;
using core::HashRng;

// Bush clumps: medium-frequency blobs so shrubs come in family groups —
// and, crossed with the forest-edge factor, break its isoline into clusters
// instead of tracing it as a string.
f32 bushClumpMask(u32 seed, f32 x, f32 z) {
    const f32 blob = terrain::noise01(seed ^ 0x452821e6u, x / 13.0f,
                                      z / 13.0f);
    return glm::smoothstep(0.52f, 0.66f, blob);
}

} // namespace

// Forest belts: broad noise thresholded, so woods come as forests and
// clearings — not confetti. Public: FarTerrain raises and darkens its
// coarse mesh with the SAME mask so the distant forest fringe continues
// the real scatter past the vegetation ring.
f32 forestMask(u32 seed, f32 x, f32 z) {
    const f32 broad =
        terrain::noise01(seed ^ 0x3c6ef372u, x / 105.0f, z / 105.0f);
    const f32 detail =
        terrain::noise01(seed ^ 0xa54ff53au, x / 28.0f, z / 28.0f);
    return glm::smoothstep(0.46f, 0.58f, broad * 0.78f + detail * 0.22f);
}

VegetationSystem::VariantBuckets scatterProps(const TerrainParams& params,
                                              i32 cx, i32 cz,
                                              f32 treeFadeEnd) {
    const f32 originX = static_cast<f32>(cx) * TerrainSystem::kChunkSize;
    const f32 originZ = static_cast<f32>(cz) * TerrainSystem::kChunkSize;
    VegetationSystem::VariantBuckets buckets;

    // `texturedRigid`: photogrammetry props with a bound albedo — the
    // NEGATIVE fade lane flags "uv = texture coords" to tree.vert, the
    // negative sway-phase lane on top says "never waves" (rocks, stumps).
    const auto place = [&](u32 firstVariant, u32 variantCount, HashRng& rng,
                           f32 x, f32 y, f32 z, f32 scaleMin, f32 scaleMax,
                           f32 fadeEnd, bool texturedRigid = false) {
        // Draw order is a CONTRACT: variant, scale, yaw, tint, phase —
        // reordering reseeds every prop in the world.
        const u32 variant =
            firstVariant +
            glm::min(static_cast<u32>(rng.next() *
                                      static_cast<f32>(variantCount)),
                     variantCount - 1);
        const f32 scale = scaleMin + rng.next() * (scaleMax - scaleMin);
        const f32 yaw = rng.next() * 6.2831853f;
        const f32 tint = rng.next();
        const f32 phase = rng.next() * 6.2831853f;
        buckets[variant].push_back({
            .positionScale = { x, y, z, scale },
            .params = { yaw,
                        tint, // tint jitter / thin key
                        texturedRigid ? -(phase + 1.0f) : phase,
                        texturedRigid ? -fadeEnd : fadeEnd },
        });
    };
    const auto candidateRng = [&](u32 salt, u32 index) {
        return HashRng { hashU32(params.seed ^ salt) ^
                         hashU32(static_cast<u32>(cx * 83492791 ^
                                                  cz * 297121507) ^
                                 index) };
    };

    // --- Trees: forest belts on gentle grassy mid-altitude ground ------------
    {
        const u32 perSide =
            static_cast<u32>(TerrainSystem::kChunkSize / kTreeSpacing);
        for (u32 i = 0; i < perSide * perSide; ++i) {
            HashRng rng = candidateRng(0x2545f491u, i);
            const f32 x = originX + (static_cast<f32>(i % perSide) +
                                     rng.next()) *
                                        kTreeSpacing;
            const f32 z = originZ + (static_cast<f32>(i / perSide) +
                                     rng.next()) *
                                        kTreeSpacing;
            const f32 forest = forestMask(params.seed, x, z);
            // Density halving done on the acceptance rather than the
            // spacing so the grid keeps its resolution (spacing x
            // sqrt(2) would truncate).
            if (forest < 0.05f || rng.next() >= forest * 0.475f) {
                continue;
            }
            const f32 h = terrain::height(params, x, z);
            const Vec3 n = terrain::normal(params, x, z);
            const f32 slope = 1.0f - n.y;
            if (h < params.seaLevel + 3.0f || slope > 0.3f ||
                terrain::underLocalWater(params, x, z, h, 1.0f)) {
                continue;
            }
            // ALTITUDE BANDS, fractions of the tree line: broadleaf
            // country low, a mixed belt, conifers up to the line (the
            // forest thins and stunts over its last stretch — a real
            // treeline dissolves), then a krummholz band of bushes just
            // past it. Grass and rock own everything above.
            const f32 h01 = h / terrain::treeLine(params);
            if (h01 >= 1.0f) {
                if (h01 < 1.15f && rng.next() < 0.35f) {
                    place(VegetationSystem::kFirstBush,
                          VegetationSystem::kBushVariants, rng, x,
                          h - 0.05f, z, 0.8f, 1.5f, 660.0f);
                }
                continue;
            }
            const f32 lineFade = 1.0f - glm::smoothstep(0.82f, 1.0f, h01);
            if (rng.next() >= lineFade) {
                continue;
            }
            u32 first = 0;
            u32 count = VegetationSystem::kBroadleafVariants;
            if (h01 >= 0.7f) { // conifer belt
                first = VegetationSystem::kBroadleafVariants;
                count = VegetationSystem::kTreeVariants -
                        VegetationSystem::kBroadleafVariants;
            } else if (h01 >= 0.45f) { // mixed belt
                count = VegetationSystem::kTreeVariants;
            }
            // Sink slightly so leaning trunks never float on slopes; the
            // offset follows the scale (their footprint is meters wide).
            const f32 stunt = 0.55f + 0.45f * lineFade;
            place(first, count, rng, x, h - 0.9f, z,
                  VegetationSystem::kTreeScaleMin * stunt,
                  VegetationSystem::kTreeScaleMax * stunt, treeFadeEnd);
        }
    }

    // --- Rocks: sparse everywhere, denser on rocky/alpine ground -------------
    {
        constexpr f32 kRockSpacing = 7.5f;
        const u32 perSide =
            static_cast<u32>(TerrainSystem::kChunkSize / kRockSpacing);
        for (u32 i = 0; i < perSide * perSide; ++i) {
            HashRng rng = candidateRng(0x8f14ab5du, i);
            const f32 x = originX + (static_cast<f32>(i % perSide) +
                                     rng.next()) *
                                        kRockSpacing;
            const f32 z = originZ + (static_cast<f32>(i / perSide) +
                                     rng.next()) *
                                        kRockSpacing;
            const f32 h = terrain::height(params, x, z);
            const Vec3 n = terrain::normal(params, x, z);
            const f32 slope = 1.0f - n.y;
            if (h < params.seaLevel + 0.5f || slope > 0.55f) {
                continue; // not underwater, not on cliff faces
            }
            const auto weights =
                terrain::materialWeightsAt(params, x, z, h, n);
            // Boulders belong to rocky and alpine ground first, meadows get
            // the occasional loner.
            const f32 chance =
                0.08f + 0.42f * weights.rock + 0.32f * weights.snow;
            if (rng.next() >= chance) {
                continue;
            }
            place(VegetationSystem::kFirstRock,
                  VegetationSystem::kRockVariants, rng, x, h - 0.10f, z,
                  0.5f, 2.0f, 660.0f, // 25% shorter reach than trees
                  true);              // photogrammetry albedo, rigid
        }
    }

    // --- Pebbles: the ground-clutter read (docs/GRASS-REDO.md) ---------------
    // The SAME boulder meshes at centimeter scale (§2.11 reuse — no new
    // variants, no new mesh): dense along the grass/rock blend band
    // (cross-scatter — stones spill into the fringe grass), sparse
    // elsewhere on rocky or bare-dirt ground. Short reach: they are a
    // near-field read.
    {
        constexpr f32 kPebbleSpacing = 1.8f;
        const u32 perSide =
            static_cast<u32>(TerrainSystem::kChunkSize / kPebbleSpacing);
        for (u32 i = 0; i < perSide * perSide; ++i) {
            HashRng rng = candidateRng(0x3e5a91c7u, i);
            const f32 x = originX + (static_cast<f32>(i % perSide) +
                                     rng.next()) *
                                        kPebbleSpacing;
            const f32 z = originZ + (static_cast<f32>(i / perSide) +
                                     rng.next()) *
                                        kPebbleSpacing;
            const f32 h = terrain::height(params, x, z);
            const Vec3 n = terrain::normal(params, x, z);
            const f32 slope = 1.0f - n.y;
            if (h < params.seaLevel + 0.5f || slope > 0.6f ||
                terrain::underLocalWater(params, x, z, h, 0.05f)) {
                continue;
            }
            const auto weights =
                terrain::materialWeightsAt(params, x, z, h, n);
            // The blend band peaks at 50/50 grass/rock; the stony (2)
            // and bare-dirt (3) ground variants carry extra loners. Pure
            // meadow keeps only a thin floor — the plants own the grassy
            // read, pebbles own the transition.
            const f32 band = 4.0f * weights.grass * weights.rock;
            f32 chance = 0.02f + 0.45f * band + 0.10f * weights.rock;
            const u32 zone = terrain::grassZoneAt(x, z).variantA;
            if (zone == 2 || zone == 3) {
                chance *= 1.6f;
            }
            if (rng.next() >= chance) {
                continue;
            }
            place(VegetationSystem::kFirstPebble,
                  VegetationSystem::kPebbleVariants, rng, x, h - 0.02f,
                  z, 0.06f, 0.25f, 90.0f, // near-field clutter reach
                  true);                  // decimated scan, rigid
        }
    }

    // --- Forest-floor debris: stumps and fallen trunks -----------------------
    {
        constexpr f32 kDebrisSpacing = 14.0f;
        const u32 perSide =
            static_cast<u32>(TerrainSystem::kChunkSize / kDebrisSpacing);
        for (u32 i = 0; i < perSide * perSide; ++i) {
            HashRng rng = candidateRng(0x7a3f19e5u, i);
            const f32 x = originX + (static_cast<f32>(i % perSide) +
                                     rng.next()) *
                                        kDebrisSpacing;
            const f32 z = originZ + (static_cast<f32>(i / perSide) +
                                     rng.next()) *
                                        kDebrisSpacing;
            // Forest interior only — debris is what a forest floor
            // leaves behind.
            const f32 forest = forestMask(params.seed, x, z);
            if (forest < 0.55f || rng.next() >= 0.16f * forest) {
                continue;
            }
            const f32 h = terrain::height(params, x, z);
            const Vec3 n = terrain::normal(params, x, z);
            if (h < params.seaLevel + 3.0f || (1.0f - n.y) > 0.35f ||
                h >= terrain::treeLine(params) ||
                terrain::underLocalWater(params, x, z, h, 0.5f)) {
                continue;
            }
            // Not through place(): the fallen trunk needs its yaw and
            // footprint BEFORE acceptance (tree clearance, ground probes).
            const u32 variant =
                VegetationSystem::kFirstDebris +
                glm::min(static_cast<u32>(
                             rng.next() *
                             static_cast<f32>(
                                 VegetationSystem::kDebrisVariants)),
                         VegetationSystem::kDebrisVariants - 1);
            const f32 scale = 1.2f + rng.next() * (2.6f - 1.2f);
            const f32 yaw = rng.next() * 6.2831853f;
            const bool log =
                variant == VegetationSystem::kFirstDebris + 1;
            // tree.vert maps local X to (cos yaw, 0, sin yaw).
            const f32 halfLen = log ? 1.1f * scale : 0.0f;
            const f32 dx = std::cos(yaw) * halfLen;
            const f32 dz = std::sin(yaw) * halfLen;
            // Trees landed first (blocks above): reject debris lying
            // into a standing trunk — bark-on-bark interpenetration
            // z-fights. Segment-to-base distance in XZ; same-chunk
            // trees only (a neighbour's tree can still graze a log
            // near the border — rare, accepted).
            bool blocked = false;
            for (u32 tv = 0;
                 tv < VegetationSystem::kTreeVariants && !blocked;
                 ++tv) {
                for (const VegetationSystem::Instance& tree :
                     buckets[tv]) {
                    const f32 tx = tree.positionScale.x - x;
                    const f32 tz = tree.positionScale.z - z;
                    const f32 along = halfLen > 0.0f
                        ? glm::clamp(tx * (dx / halfLen) +
                                         tz * (dz / halfLen),
                                     -halfLen, halfLen)
                        : 0.0f;
                    const f32 ox = tx - (halfLen > 0.0f
                                             ? along * (dx / halfLen)
                                             : 0.0f);
                    const f32 oz = tz - (halfLen > 0.0f
                                             ? along * (dz / halfLen)
                                             : 0.0f);
                    // Bark radius grows with the tree scale (the
                    // collision box uses 0.28 x scale) + log girth.
                    const f32 r = 0.7f + 0.3f * tree.positionScale.w;
                    if (ox * ox + oz * oz < r * r) {
                        blocked = true;
                        break;
                    }
                }
            }
            if (blocked) {
                continue;
            }
            f32 y = h - 0.06f;
            if (log) {
                // Props carry yaw only (no slope alignment) — probe the
                // ground along the axis, reject ground a straight log
                // cannot lie on, seat on the CREST (a half-buried log
                // leaves a grazing surface that z-fights the terrain).
                const f32 hA = terrain::height(params, x + dx, z + dz);
                const f32 hB = terrain::height(params, x - dx, z - dz);
                const f32 hi = glm::max(h, glm::max(hA, hB));
                const f32 lo = glm::min(h, glm::min(hA, hB));
                if (hi - lo > 0.30f * scale) {
                    continue;
                }
                y = hi - 0.05f * scale;
            }
            buckets[variant].push_back({
                .positionScale = { x, y, z, scale },
                .params = { yaw, rng.next(),
                            -(rng.next() * 6.2831853f + 1.0f),
                            -300.0f }, // textured rigid (scanned debris)
            });
        }
    }

    // --- Plants: photoreal accents over the blade meadow ---------------------
    // (docs/GRASS-REDO.md palier 2.) Textured cutout scans, picked by
    // HABITAT: fern inside forests, shrub on their edges, dandelion in
    // open meadow, tall grass anywhere grassy. The NEGATIVE fade lane
    // flags "textured" to tree.vert/frag (uv = texture coords, height
    // sway); reach stays short — they are a near-field read like the
    // pebbles.
    {
        // Density leans on the shader's distance ramp (thin key): the
        // near field is dense, the far field a deterministic subset.
        constexpr f32 kPlantSpacing = 3.0f;
        constexpr f32 kPlantFade = 60.0f;
        const u32 perSide =
            static_cast<u32>(TerrainSystem::kChunkSize / kPlantSpacing);
        for (u32 i = 0; i < perSide * perSide; ++i) {
            HashRng rng = candidateRng(0x5c17ba31u, i);
            const f32 x = originX + (static_cast<f32>(i % perSide) +
                                     rng.next()) *
                                        kPlantSpacing;
            const f32 z = originZ + (static_cast<f32>(i / perSide) +
                                     rng.next()) *
                                        kPlantSpacing;
            const f32 h = terrain::height(params, x, z);
            const Vec3 n = terrain::normal(params, x, z);
            if (h < params.seaLevel + 0.5f || (1.0f - n.y) > 0.4f ||
                h >= terrain::treeLine(params) ||
                terrain::underLocalWater(params, x, z, h, 0.1f)) {
                continue;
            }
            if (terrain::materialWeightsAt(params, x, z, h, n).grass <
                0.55f) {
                continue;
            }
            const f32 forest = forestMask(params.seed, x, z);
            // Habitat pick; each keeps its own acceptance so densities
            // tune independently.
            u32 species = 0; // tall grass
            f32 accept = 0.18f;
            if (forest > 0.4f) {
                species = 1; // fern
                accept = 0.32f;
            } else if (forest > 0.15f) {
                species = 3; // shrub
                accept = 0.14f;
            } else if (rng.next() < 0.35f) {
                species = 2; // dandelion
                accept = 0.20f;
            }
            // COLONY noise (~12 m, per species): nature grows in
            // patches — dense hearts, empty clearings. The mass tier
            // below shares the same field, so carpets and heroes agree.
            const f32 colony = glm::smoothstep(
                0.45f, 0.75f,
                terrain::noise01(params.seed ^ (0xa11c03du + species),
                                 x * 0.08f, z * 0.08f));
            accept *= 2.0f * colony;
            // Worn/dirt ground variants carry fewer plants.
            const u32 zone = terrain::grassZoneAt(x, z).variantA;
            if (zone == 1 || zone == 3) {
                accept *= 0.5f;
            }
            if (rng.next() >= accept) {
                continue;
            }
            buckets[VegetationSystem::kFirstPlant + species].push_back({
                .positionScale = { x, h - 0.03f, z,
                                   0.9f + rng.next() * 0.6f },
                .params = { rng.next() * 6.2831853f, rng.next(),
                            rng.next() * 6.2831853f,
                            -kPlantFade }, // negative = textured cutout
            });
        }
    }

    // --- Mass tier: the carpet under the hero plants -------------------------
    // Cheap clones (~200 tris) of the same species, high density, short
    // reach, SAME colony field — the continuous understory read; the
    // heroes above become focal points sitting on it.
    {
        constexpr f32 kMassSpacing = 2.0f;
        constexpr f32 kMassFade = 35.0f;
        const u32 perSide =
            static_cast<u32>(TerrainSystem::kChunkSize / kMassSpacing);
        for (u32 i = 0; i < perSide * perSide; ++i) {
            HashRng rng = candidateRng(0x9dd23b71u, i);
            const f32 x = originX + (static_cast<f32>(i % perSide) +
                                     rng.next()) *
                                        kMassSpacing;
            const f32 z = originZ + (static_cast<f32>(i / perSide) +
                                     rng.next()) *
                                        kMassSpacing;
            const f32 h = terrain::height(params, x, z);
            const Vec3 n = terrain::normal(params, x, z);
            if (h < params.seaLevel + 0.5f || (1.0f - n.y) > 0.4f ||
                h >= terrain::treeLine(params) ||
                terrain::underLocalWater(params, x, z, h, 0.1f)) {
                continue;
            }
            if (terrain::materialWeightsAt(params, x, z, h, n).grass <
                0.55f) {
                continue;
            }
            const f32 forest = forestMask(params.seed, x, z);
            u32 species = 0;
            f32 accept = 0.35f;
            if (forest > 0.4f) {
                species = 1; // fern carpet
                accept = 0.55f;
            } else if (forest > 0.15f) {
                species = 3;
                accept = 0.25f;
            } else if (rng.next() < 0.35f) {
                species = 2;
                accept = 0.35f;
            }
            const f32 colony = glm::smoothstep(
                0.45f, 0.75f,
                terrain::noise01(params.seed ^ (0xa11c03du + species),
                                 x * 0.08f, z * 0.08f));
            accept *= 2.0f * colony;
            const u32 zone = terrain::grassZoneAt(x, z).variantA;
            if (zone == 1 || zone == 3) {
                accept *= 0.5f;
            }
            if (rng.next() >= accept) {
                continue;
            }
            buckets[VegetationSystem::kFirstMass + species].push_back({
                .positionScale = { x, h - 0.03f, z,
                                   0.7f + rng.next() * 0.6f },
                .params = { rng.next() * 6.2831853f, rng.next(),
                            rng.next() * 6.2831853f, -kMassFade },
            });
        }
    }

    // --- Bushes: grassy ground, biased toward forest edges -------------------
    {
        constexpr f32 kBushSpacing = 5.0f;
        const u32 perSide =
            static_cast<u32>(TerrainSystem::kChunkSize / kBushSpacing);
        for (u32 i = 0; i < perSide * perSide; ++i) {
            HashRng rng = candidateRng(0xc1d1f0adu, i);
            const f32 x = originX + (static_cast<f32>(i % perSide) +
                                     rng.next()) *
                                        kBushSpacing;
            const f32 z = originZ + (static_cast<f32>(i / perSide) +
                                     rng.next()) *
                                        kBushSpacing;
            const f32 h = terrain::height(params, x, z);
            const Vec3 n = terrain::normal(params, x, z);
            // 0.5 keeps bushes alive in the alpine biome
            // (grassPresence 0.6); arid scrub stays excluded.
            if (terrain::materialWeightsAt(params, x, z, h, n).grass <
                    0.5f ||
                terrain::underLocalWater(params, x, z, h, 0.3f)) {
                continue;
            }
            // Clumps gate everything (bushes come in family groups); the
            // forest-edge factor then biases WHICH clumps are lush, without
            // tracing the treeline as a string.
            const f32 clump = bushClumpMask(params.seed, x, z);
            if (clump < 0.05f) {
                continue;
            }
            const f32 forest = forestMask(params.seed, x, z);
            const f32 edge = forest * (1.0f - forest) * 4.0f;
            if (rng.next() >= clump * (0.35f + 0.65f * edge)) {
                continue;
            }
            place(VegetationSystem::kFirstBush,
                  VegetationSystem::kBushVariants, rng, x, h - 0.05f, z,
                  0.7f, 1.3f, 660.0f); // small silhouettes: rock reach
        }
    }
    return buckets;
}

u32 VegetationSystem::InstancePool::alloc(u32 size) {
    for (size_t i = 0; i < freeBlocks.size(); ++i) {
        if (freeBlocks[i].size < size) {
            continue; // first fit
        }
        const u32 offset = freeBlocks[i].offset;
        if (freeBlocks[i].size == size) {
            freeBlocks.erase(freeBlocks.begin() +
                             static_cast<std::ptrdiff_t>(i));
        } else {
            freeBlocks[i].offset += size;
            freeBlocks[i].size -= size;
        }
        return offset;
    }
    return kNoOffset;
}

void VegetationSystem::InstancePool::tick() {
    for (const Block& cooled : cooling[1]) {
        // Sorted insert + coalesce with both neighbors.
        auto it = std::lower_bound(
            freeBlocks.begin(), freeBlocks.end(), cooled,
            [](const Block& a, const Block& b) {
                return a.offset < b.offset;
            });
        it = freeBlocks.insert(it, cooled);
        if (it + 1 != freeBlocks.end() &&
            it->offset + it->size == (it + 1)->offset) {
            it->size += (it + 1)->size;
            it = freeBlocks.erase(it + 1) - 1;
        }
        if (it != freeBlocks.begin() &&
            (it - 1)->offset + (it - 1)->size == it->offset) {
            (it - 1)->size += it->size;
            freeBlocks.erase(it);
        }
    }
    cooling[1] = std::move(cooling[0]);
    cooling[0].clear();
}

void VegetationSystem::create(rhi::Device& device, ShaderLibrary& shaders,
                              core::JobSystem& jobSystem, u32 terrainSeed) {
    streamer.create(jobSystem);
    meshSeed = terrainSeed;
    instancePool.buffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = u64(InstancePool::kCapacity) * sizeof(Instance) },
        nullptr) };
    instancePool.freeBlocks = { { 0, InstancePool::kCapacity } };
    createVariantMeshes(device, terrainSeed);
    rebuildLeafMask(device);
    shaders.load(kTreeShader, { { "FrameUbo", 0 } },
                 { { "uLeafMask", 0 },
                   { "uShadowMap", 1 },
                   { "uPropNormal", 12 },
                   { "uTerrainShade0", 4 },
                   { "uBark", 13 },
                   { "uBarkNrm", 14 } });
    buildPipeline(device, shaders);
    shaders.load(kPropCasterShader, { { "FrameUbo", 0 }, { "ShadowUbo", 1 } },
                 { { "uLeafMask", 0 } });
    buildCasterPipeline(device, shaders);
}

void VegetationSystem::rebuildLeafMask(rhi::Device& device) {
    constexpr u32 kMaskSize = 256;
    // Full mip chain when the device can fill it — alpha-tested cards
    // shrink to a few pixels at the ring edge; base-level-only sampling
    // there is pure shimmer.
    const bool mips = device.caps().mipmapGeneration;
    const u32 mipLevels =
        mips ? 1 + static_cast<u32>(std::log2(static_cast<f32>(kMaskSize)))
             : 1;
    // ATLAS, kLeafStyleCount tiles in a row: each SPECIES claims the
    // slot `leafStyle` and rasters it from its own leaf params + shape;
    // the card's flag bias picks the tile in tree.vert /
    // shadow_prop.vert. Slot 0 defaults to the live global params;
    // unclaimed slots copy slot 0 (a mis-set style never shows holes).
    // The per-slot SEASON table (autumn tint + seasonality) rides the
    // same claim pass — the shaders read it from the frame UBO.
    array<std::optional<ColonizedTreeParams>, kLeafStyleCount> claims {};
    claims[0] = colonizedTreeParams;
    const auto claim = [&](const TreeSpecies& species) {
        if (!species.colonized) {
            return;
        }
        const u32 slot = static_cast<u32>(
            glm::clamp(species.params.leafStyle, 0, kLeafStyleCount - 1));
        claims[slot] = species.params;
    };
    for (u32 i = 0; i < kTreeVariants; ++i) {
        if (treeSpecies[i]) {
            claim(*treeSpecies[i]);
        }
    }
    if (bushSpecies) {
        claim(*bushSpecies);
    }
    vector<u8> pixels(static_cast<size_t>(kMaskSize) * kMaskSize *
                      kLeafStyleCount * 4);
    const u32 atlasWidth = kMaskSize * kLeafStyleCount;
    for (u32 slot = 0; slot < static_cast<u32>(kLeafStyleCount); ++slot) {
        const ColonizedTreeParams& p =
            claims[slot] ? *claims[slot] : *claims[0];
        leafSeasonTable[slot] = { p.autumnTint.x, p.autumnTint.y,
                                  p.autumnTint.z, p.seasonality };
        const vector<u8> tile = generateLeafMaskPixels(
            kMaskSize, meshSeed, p, claims[slot] ? p.leafShape : 0);
        for (u32 row = 0; row < kMaskSize; ++row) {
            std::memcpy(&pixels[(static_cast<size_t>(row) * atlasWidth +
                                 slot * kMaskSize) *
                                4],
                        &tile[static_cast<size_t>(row) * kMaskSize * 4],
                        static_cast<size_t>(kMaskSize) * 4);
        }
    }
    leafMask = { device, device.createTexture(
        { .width = atlasWidth,
          .height = kMaskSize,
          .mipLevels = mipLevels,
          .format = rhi::TextureFormat::RGBA8,
          .filter = rhi::FilterMode::Linear },
        pixels.data()) };
    if (mips) {
        device.generateMipmaps(leafMask.get());
    }
    if (leafMaskSampler.get().id == 0) {
        leafMaskSampler = { device,
                            device.createSampler(
                                { .mipmapFilter = mips }) }; // linear clamp
    }
    leafMaskGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = leafMask.get(),
                         .sampler = leafMaskSampler.get() },
                       // Same layout as the textured-prop groups: a flat
                       // normal fills the map slot (cards don't use it),
                       // and the bark slot (inert without the flag).
                       { .binding = 12,
                         .texture = flatNormalHandle(device),
                         .sampler = leafMaskSampler.get() },
                       { .binding = 13,
                         .texture = flatNormalHandle(device),
                         .sampler = leafMaskSampler.get() },
                       { .binding = 14,
                         .texture = flatNormalHandle(device),
                         .sampler = leafMaskSampler.get() } } }) };
    // Tree bark groups reference the leaf mask just recreated.
    rebuildTreeBarkGroups(device);
}

void VegetationSystem::createVariantMeshes(rhi::Device& device,
                                           u32 terrainSeed) {
    // Ambient grounding is BAKED into the vertex
    // colors — canopy interiors and rock creases darken with zero
    // runtime cost. Content-keyed DISK CACHE (same store as the glTF
    // bakes): these 17 synchronous bakes cost ~a minute in an
    // unoptimized Debug build — once per geometry now, then loads.
    // [cpp-tuning] strengths below.
    const std::filesystem::path aoCacheDir =
        platform::executableDir() / "data" / "cache" / "ao";
    const auto baked = [&](MeshData mesh, f32 strength) {
        assets::applyContentKeyedVertexAo(mesh, aoCacheDir, strength);
        return mesh;
    };
    for (u32 i = 0; i < kVariantCount; ++i) {
        if (const auto it = meshOverrides.find(i);
            it != meshOverrides.end()) {
            uploadVariantMesh(device, i, baked(it->second.high, 0.55f));
            if (!it->second.low.vertices.empty()) {
                uploadLowDetailMesh(device, i,
                                    baked(it->second.low, 0.55f));
            }
            if (!it->second.ultra.vertices.empty()) {
                uploadUltraDetailMesh(device, i,
                                      baked(it->second.ultra, 0.55f));
            }
            continue;
        }
        const u32 seed = hashU32(terrainSeed) + i * 977u;
        if (i < kFirstRock) {
            const TreeSpecies species = speciesFor(i);
            const auto tree = [&](u32 lod) {
                return species.colonized
                           ? generateColonizedTree(seed, lod,
                                                   species.params)
                           : generateTree(seed, lod, species.lobes);
            };
            uploadVariantMesh(device, i, baked(tree(2), 0.6f));
            uploadLowDetailMesh(device, i, baked(tree(1), 0.6f));
            // Bare-icosahedron lobes (~150 tris/tree) for the far
            // ring — same seed, same composition, facets invisible there.
            uploadUltraDetailMesh(device, i, baked(tree(0), 0.6f));
            if (species.colonized) {
                // Far-cascade caster: solid metaball blobs, no AO bake
                // (depth-only) — see generateColonizedTreeShadowProxy.
                uploadShadowProxyMesh(
                    device, i,
                    generateColonizedTreeShadowProxy(seed,
                                                     species.params));
            }
        } else if (i < kFirstBush || i >= kFirstDebris) {
            // Rocks — and the debris slots' PLACEHOLDER until the scene's
            // scanned-prop overrides land (meshOverrides above wins).
            uploadVariantMesh(device, i, baked(generateRock(seed), 0.5f));
        } else {
            if (bushSpecies) {
                // A knee-high colonized canopy: real branches + card
                // foliage, three LODs like the trees.
                const auto bush = [&](u32 lod) {
                    return bushSpecies->colonized
                               ? generateColonizedTree(
                                     seed, lod, bushSpecies->params)
                               : generateTree(seed, lod,
                                              bushSpecies->lobes);
                };
                uploadVariantMesh(device, i, baked(bush(2), 0.55f));
                uploadLowDetailMesh(device, i, baked(bush(1), 0.55f));
                uploadUltraDetailMesh(device, i, baked(bush(0), 0.55f));
            } else {
                uploadVariantMesh(device, i,
                                  baked(generateBush(seed), 0.55f));
            }
        }
    }
    // Textured plants: the reset above dropped their albedo bind groups
    // with the meshes — re-create them from the kept CPU copies.
    for (const auto& [variant, albedo] : albedoOverrides) {
        (void)albedo;
        uploadVariantAlbedo(device, variant);
    }
    rebuildTreeBarkGroups(device);
}

VegetationSystem::TreeSilhouette VegetationSystem::treeSilhouette() const {
    Vec3 sum { 0.0f };
    u32 count = 0;
    for (const Vec3& bounds : treeBounds) {
        if (bounds.x > 0.1f) {
            sum += bounds;
            ++count;
        }
    }
    if (count == 0) {
        return {}; // defaults until the first variant lands
    }
    const Vec3 mean = sum / static_cast<f32>(count);
    const f32 scale = (kTreeScaleMin + kTreeScaleMax) * 0.5f;
    return { mean.x * scale,
             glm::clamp(2.0f * mean.y / glm::max(mean.x, 0.1f), 0.4f,
                        1.4f),
             glm::clamp(mean.z / glm::max(mean.x, 0.1f), 0.05f, 0.7f) };
}

void VegetationSystem::uploadVariantMesh(rhi::Device& device, u32 variant,
                                         const MeshData& mesh) {
    if (variant < kTreeVariants && !mesh.vertices.empty()) {
        // Silhouette measurement (see TreeSilhouette): height, widest
        // radial extent, and where the crown starts (first height with
        // real width — everything below is bare trunk).
        f32 maxY = 0.0f;
        f32 maxR = 0.0f;
        for (const MeshVertex& v : mesh.vertices) {
            maxY = glm::max(maxY, v.position.y);
            maxR = glm::max(maxR, glm::length(Vec2 { v.position.x,
                                                     v.position.z }));
        }
        f32 crownStart = maxY;
        for (const MeshVertex& v : mesh.vertices) {
            if (glm::length(Vec2 { v.position.x, v.position.z }) >
                maxR * 0.35f) {
                crownStart = glm::min(crownStart, v.position.y);
            }
        }
        treeBounds[variant] = { maxY, maxR, crownStart };
    }
    variantMeshes[variant].indexCount =
        static_cast<u32>(mesh.indices.size());
    variantMeshes[variant].vertexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = mesh.vertices.size() * sizeof(MeshVertex) },
        mesh.vertices.data()) };
    variantMeshes[variant].indexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = mesh.indices.size() * sizeof(u32) },
        mesh.indices.data()) };
}

void VegetationSystem::uploadLowDetailMesh(rhi::Device& device, u32 variant,
                                           const MeshData& mesh) {
    variantMeshes[variant].lowIndexCount =
        static_cast<u32>(mesh.indices.size());
    variantMeshes[variant].lowVertexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = mesh.vertices.size() * sizeof(MeshVertex) },
        mesh.vertices.data()) };
    variantMeshes[variant].lowIndexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = mesh.indices.size() * sizeof(u32) },
        mesh.indices.data()) };
}

void VegetationSystem::uploadUltraDetailMesh(rhi::Device& device, u32 variant,
                                             const MeshData& mesh) {
    variantMeshes[variant].ultraIndexCount =
        static_cast<u32>(mesh.indices.size());
    variantMeshes[variant].ultraVertexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = mesh.vertices.size() * sizeof(MeshVertex) },
        mesh.vertices.data()) };
    variantMeshes[variant].ultraIndexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = mesh.indices.size() * sizeof(u32) },
        mesh.indices.data()) };
}

void VegetationSystem::uploadShadowProxyMesh(rhi::Device& device,
                                             u32 variant,
                                             const MeshData& mesh) {
    variantMeshes[variant].casterIndexCount =
        static_cast<u32>(mesh.indices.size());
    variantMeshes[variant].casterVertexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = mesh.vertices.size() * sizeof(MeshVertex) },
        mesh.vertices.data()) };
    variantMeshes[variant].casterIndexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = mesh.indices.size() * sizeof(u32) },
        mesh.indices.data()) };
}

void VegetationSystem::overrideVariantMesh(rhi::Device& device, u32 variant,
                                           MeshData mesh, MeshData low,
                                           MeshData ultra) {
    if (variant >= kVariantCount || mesh.vertices.empty() ||
        mesh.indices.empty()) {
        return;
    }
    // U3-7: the reset frees every detail level through its wrappers.
    variantMeshes[variant] = {};
    uploadVariantMesh(device, variant, mesh);
    if (!low.vertices.empty() && !low.indices.empty()) {
        uploadLowDetailMesh(device, variant, low);
    }
    if (!ultra.vertices.empty() && !ultra.indices.empty()) {
        uploadUltraDetailMesh(device, variant, ultra);
    }
    meshOverrides[variant] = { std::move(mesh), std::move(low),
                               std::move(ultra) };
}

void VegetationSystem::setVariantAlbedo(rhi::Device& device, u32 variant,
                                        u32 width, u32 height,
                                        vector<u8> rgba, u32 normalWidth,
                                        u32 normalHeight,
                                        vector<u8> normalRgba) {
    if (variant >= kVariantCount || rgba.size() <
                                        static_cast<size_t>(width) *
                                            height * 4) {
        return;
    }
    if (normalRgba.size() <
        static_cast<size_t>(normalWidth) * normalHeight * 4) {
        normalWidth = 0;
        normalHeight = 0;
        normalRgba.clear();
    }
    albedoOverrides[variant] = { width,        height,
                                 std::move(rgba), normalWidth,
                                 normalHeight, std::move(normalRgba) };
    uploadVariantAlbedo(device, variant);
}

rhi::TextureHandle VegetationSystem::flatNormalHandle(rhi::Device& device) {
    if (flatNormal.get().id == 0) {
        const u8 up[4] = { 128, 128, 255, 255 };
        flatNormal = { device, device.createTexture(
                                   { .width = 1, .height = 1 }, up) };
    }
    return flatNormal.get();
}

void VegetationSystem::setBarkTextures(rhi::Device& device,
                                       BarkImage oakAlbedo,
                                       BarkImage oakNrmHeight,
                                       BarkImage pineAlbedo,
                                       BarkImage pineNrmHeight) {
    barkImages[0] = std::move(oakAlbedo);
    barkImages[1] = std::move(pineAlbedo);
    barkNrmImages[0] = std::move(oakNrmHeight);
    barkNrmImages[1] = std::move(pineNrmHeight);
    const bool mips = device.caps().mipmapGeneration;
    const auto upload = [&](const BarkImage& src, bool srgb,
                            rhi::UniqueTexture& out) {
        if (src.rgba.size() <
            static_cast<size_t>(src.width) * src.height * 4) {
            return;
        }
        const u32 mipLevels =
            mips ? 1 + static_cast<u32>(std::log2(static_cast<f32>(
                       glm::max(src.width, src.height))))
                 : 1;
        out = { device, device.createTexture(
            { .width = src.width,
              .height = src.height,
              .mipLevels = mipLevels,
              .format = srgb ? rhi::TextureFormat::SRGBA8
                             : rhi::TextureFormat::RGBA8,
              .filter = rhi::FilterMode::Linear },
            src.rgba.data()) };
        if (mips) {
            device.generateMipmaps(out.get());
        }
    };
    for (u32 b = 0; b < 2; ++b) {
        upload(barkImages[b], true, barkTextures[b]);
        upload(barkNrmImages[b], false, barkNrmTextures[b]);
    }
    if (barkSampler.get().id == 0) {
        barkSampler = { device, device.createSampler(
            { .mipmapFilter = mips,
              .addressU = rhi::AddressMode::Repeat,
              .addressV = rhi::AddressMode::Repeat }) };
    }
    rebuildTreeBarkGroups(device);
}

void VegetationSystem::rebuildTreeBarkGroups(rhi::Device& device) {
    if (!barkLoaded() || leafMask.get().id == 0) {
        return;
    }
    for (u32 i = 0; i < kTreeVariants; ++i) {
        const u32 pick = glm::min<u32>(variantBark[i], 1u);
        const rhi::TextureHandle bark =
            barkTextures[pick].get().id != 0 ? barkTextures[pick].get()
                                             : barkTextures[0].get();
        const rhi::TextureHandle nrm =
            barkNrmTextures[pick].get().id != 0
                ? barkNrmTextures[pick].get()
                : flatNormalHandle(device);
        variantMeshes[i].albedoGroup = { device, device.createBindGroup(
            { .entries = { { .binding = 0,
                             .texture = leafMask.get(),
                             .sampler = leafMaskSampler.get() },
                           { .binding = 12,
                             .texture = flatNormalHandle(device),
                             .sampler = leafMaskSampler.get() },
                           { .binding = 13,
                             .texture = bark,
                             .sampler = barkSampler.get() },
                           { .binding = 14,
                             .texture = nrm,
                             .sampler = barkSampler.get() } } }) };
    }
}

void VegetationSystem::uploadVariantAlbedo(rhi::Device& device,
                                           u32 variant) {
    const auto it = albedoOverrides.find(variant);
    if (it == albedoOverrides.end()) {
        return;
    }
    const AlbedoOverride& src = it->second;
    VariantMesh& mesh = variantMeshes[variant];
    const bool mips = device.caps().mipmapGeneration;
    const u32 mipLevels =
        mips ? 1 + static_cast<u32>(std::log2(static_cast<f32>(
                   glm::max(src.width, src.height))))
             : 1;
    mesh.albedo = { device, device.createTexture(
        { .width = src.width,
          .height = src.height,
          .mipLevels = mipLevels,
          .format = rhi::TextureFormat::SRGBA8,
          .filter = rhi::FilterMode::Linear },
        src.rgba.data()) };
    if (mips) {
        device.generateMipmaps(mesh.albedo.get());
    }
    if (leafMaskSampler.get().id == 0) {
        leafMaskSampler = { device, device.createSampler(
                                        { .mipmapFilter = mips }) };
    }
    // Normal map (linear RGBA8, GL +Y) or the shared flat fallback.
    rhi::TextureHandle normalTex = flatNormalHandle(device);
    if (!src.normalRgba.empty()) {
        const u32 nMips =
            mips ? 1 + static_cast<u32>(std::log2(static_cast<f32>(
                       glm::max(src.normalWidth, src.normalHeight))))
                 : 1;
        mesh.normalMap = { device, device.createTexture(
            { .width = src.normalWidth,
              .height = src.normalHeight,
              .mipLevels = nMips,
              .format = rhi::TextureFormat::RGBA8,
              .filter = rhi::FilterMode::Linear },
            src.normalRgba.data()) };
        if (mips) {
            device.generateMipmaps(mesh.normalMap.get());
        }
        normalTex = mesh.normalMap.get();
    }
    // Same layout as leafMaskGroup — the tree pipeline binds either
    // interchangeably as group 1 (binding 7 = the bark slot, dummy here:
    // textured props never raise the bark flag).
    mesh.albedoGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = mesh.albedo.get(),
                         .sampler = leafMaskSampler.get() },
                       { .binding = 12,
                         .texture = normalTex,
                         .sampler = leafMaskSampler.get() },
                       { .binding = 13,
                         .texture = mesh.albedo.get(),
                         .sampler = leafMaskSampler.get() },
                       { .binding = 14,
                         .texture = normalTex,
                         .sampler = leafMaskSampler.get() } } }) };
}

void VegetationSystem::destroyVariantMeshes(rhi::Device& device) {
    (void)device; // U3-7: assignment frees through the wrappers
    for (VariantMesh& variant : variantMeshes) {
        variant = {};
    }
}

VegetationSystem::TreeSpecies VegetationSystem::speciesFor(
    u32 slot) const {
    if (slot < kTreeVariants && treeSpecies[slot]) {
        return *treeSpecies[slot];
    }
    return { colonizationTrees, lobeTreeParams, colonizedTreeParams };
}

void VegetationSystem::reseedVariantMeshesAsync(core::JobSystem& jobs,
                                                u32 seed) {
    reseedJobs = &jobs;
    if (reseedJob) {
        // Coalesce: the in-flight job lands first, then relaunches with
        // the LATEST params (knob storms collapse into two passes).
        reseedQueued = true;
        reseedQueuedSeed = seed;
        return;
    }
    meshSeed = seed;
    auto job = std::make_shared<ReseedJob>();
    job->seed = seed;
    job->total = 0;
    for (u32 i = 0; i < kTreeVariants; ++i) {
        job->species[i] = speciesFor(i);
        job->total += job->species[i].colonized ? 4u : 3u;
    }
    job->aoCacheDir =
        platform::executableDir() / "data" / "cache" / "ao";
    reseedJob = job;
    jobs.enqueue([job] {
        // Pure CPU (mesh generation + content-keyed AO bake) — the
        // MeshCache decode-worker pattern; only the sptr is captured.
        const auto baked = [&](MeshData mesh) {
            assets::applyContentKeyedVertexAo(mesh, job->aoCacheDir, 0.6f);
            return mesh;
        };
        for (u32 i = 0; i < kTreeVariants; ++i) {
            const TreeSpecies& species = job->species[i];
            const u32 variantSeed = hashU32(job->seed) + i * 977u;
            for (u32 lod = 0; lod < 3; ++lod) {
                job->lods[i][lod] = baked(
                    species.colonized
                        ? generateColonizedTree(variantSeed, lod,
                                                species.params)
                        : generateTree(variantSeed, lod, species.lobes));
                job->completed.fetch_add(1, std::memory_order_release);
            }
            if (species.colonized) {
                job->casters[i] = generateColonizedTreeShadowProxy(
                    variantSeed, species.params);
                job->completed.fetch_add(1, std::memory_order_release);
            }
        }
        job->done.store(true, std::memory_order_release);
    });
}

f32 VegetationSystem::reseedProgress() const {
    if (!reseedJob) {
        return 1.0f;
    }
    return static_cast<f32>(
               reseedJob->completed.load(std::memory_order_acquire)) /
           static_cast<f32>(glm::max(reseedJob->total, 1u));
}

void VegetationSystem::pumpReseed(rhi::Device& device) {
    if (!reseedJob || !reseedJob->done.load(std::memory_order_acquire)) {
        return;
    }
    const sptr<ReseedJob> job = std::move(reseedJob);
    for (u32 i = 0; i < kTreeVariants; ++i) {
        if (meshOverrides.contains(i)) {
            continue; // authored meshes keep their single level
        }
        variantMeshes[i] = {}; // clears a stale caster on an algo switch
        uploadVariantMesh(device, i, job->lods[i][2]);
        uploadLowDetailMesh(device, i, job->lods[i][1]);
        uploadUltraDetailMesh(device, i, job->lods[i][0]);
        if (job->species[i].colonized) {
            uploadShadowProxyMesh(device, i, job->casters[i]);
        }
    }
    rebuildLeafMask(device); // its knobs ride the same panel
    if (reseedQueued) {
        reseedQueued = false;
        if (reseedJobs != nullptr) {
            reseedVariantMeshesAsync(*reseedJobs, reseedQueuedSeed);
        }
    }
}

void VegetationSystem::setShowcase(rhi::Device& device,
                                   const vector<Instance>& list) {
    showcaseInstances.reset();
    showcaseCount = static_cast<u32>(list.size());
    if (list.empty()) {
        return;
    }
    showcaseInstances = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = list.size() * sizeof(Instance) },
        list.data()) };
}

void VegetationSystem::destroy(rhi::Device& device) {
    streamer.invalidateAll([](Chunk&) {});
    instancePool = {};
    instances = 0;
    showcaseInstances.reset();
    showcaseCount = 0;
    reseedJob.reset(); // a worker still running keeps its own copy
    reseedQueued = false;
    pipeline.reset();
    casterPipeline.reset();
    leafMaskGroup.reset();
    leafMaskSampler.reset();
    leafMask.reset();
    destroyVariantMeshes(device);
}

void VegetationSystem::regenerate(rhi::Device& device, u32 terrainSeed) {
    streamer.invalidateAll([&](Chunk& chunk) {
        if (chunk.resident && chunk.total > 0 &&
            chunk.poolOffset != kNoOffset) {
            instancePool.free(chunk.poolOffset, chunk.total);
        }
    });
    instances = 0;
    meshSeed = terrainSeed;
    destroyVariantMeshes(device);
    createVariantMeshes(device, terrainSeed);
}

void VegetationSystem::invalidateChunks(rhi::Device& device,
                                        const vector<u64>& keys) {
    (void)device;
    // The shared variant meshes are height-independent — only per-chunk scatter
    // is dropped so props re-seat on the new terrain.
    for (const u64 key : keys) {
        const auto it = streamer.chunks.find(key);
        if (it == streamer.chunks.end()) {
            continue;
        }
        if (!it->second.resident) {
            // Build in flight against the OLD terrain: mark it — the
            // pump discards the landing payload and re-requests.
            it->second.stale = true;
            continue;
        }
        if (it->second.total > 0 && it->second.poolOffset != kNoOffset) {
            instancePool.free(it->second.poolOffset, it->second.total);
        }
        instances -= it->second.total;
        // update() re-requests + re-scatters with new heights.
        streamer.chunks.erase(it);
    }
}

void VegetationSystem::update(rhi::Device& device, const TerrainParams& params,
                              const Vec3& cameraPos, bool holdRequests) {
    frameIndices = 0; // the frame's draw*() calls sum into these
    frameHighInstances = 0;
    frameLowInstances = 0;
    frameUltraInstances = 0;
    pumpReseed(device); // async reseed landing point (main thread)
    if (barkGroupsDirty) {
        barkGroupsDirty = false;
        rebuildTreeBarkGroups(device);
    }
    if (showcaseCount != 0) {
        return; // showcase replaces the streamed scatter entirely
    }
    instancePool.tick(); // two-frame-cooled blocks become reusable
    // Budgeted uploads (U3-1: ring mechanics in ChunkStreamer; this lambda
    // is the vegetation-specific accept — variant packing + GPU upload
    // into the pooled instance buffer).
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
        vector<Instance> packed;
        chunk.giProps.clear();
        for (u32 v = 0; v < kVariantCount; ++v) {
            chunk.firstInstance[v] = static_cast<u32>(packed.size());
            chunk.counts[v] = static_cast<u32>(built.payload[v].size());
            packed.insert(packed.end(), built.payload[v].begin(),
                          built.payload[v].end());
            if (v >= kFirstPlant) {
                continue; // plants: no GI injection boxes (small cutouts)
            }
            // The compact CPU copy the GI injection boxes.
            const u8 kind = v < kFirstRock                          ? 0
                            : (v < kFirstBush || v >= kFirstDebris) ? 1
                                                                    : 2;
            for (const Instance& instance : built.payload[v]) {
                chunk.giProps.push_back(
                    { Vec3 { instance.positionScale },
                      instance.positionScale.w, kind });
            }
        }
        chunk.total = static_cast<u32>(packed.size());
        if (chunk.total > 0) {
            const u32 offset = instancePool.alloc(chunk.total);
            if (offset == kNoOffset) {
                LOG_WARN("VegetationSystem: instance pool full — chunk "
                         "scatter dropped");
                streamer.chunks.erase(it); // re-detected and re-requested
                return false;
            }
            device.updateBuffer(instancePool.buffer.get(), packed.data(),
                                packed.size() * sizeof(Instance),
                                u64(offset) * sizeof(Instance));
            chunk.poolOffset = offset;
            chunk.minY = packed[0].positionScale.y;
            chunk.maxY = chunk.minY;
            for (const Instance& instance : packed) {
                chunk.minY = glm::min(chunk.minY, instance.positionScale.y);
                chunk.maxY = glm::max(chunk.maxY, instance.positionScale.y);
            }
        }
        chunk.resident = true;
        instances += chunk.total;
        return true;
    });

    // (giProps live in the Chunk: eviction frees them with it.)

    // Request missing chunks — nearest first, budgeted (the rest is
    // re-detected next frame; the state IS the queue). See TerrainSystem:
    // the unbudgeted ring edge was part of the fast-travel stutter.
    const i32 camCx = chunkCoordOf(cameraPos.x, TerrainSystem::kChunkSize);
    const i32 camCz = chunkCoordOf(cameraPos.z, TerrainSystem::kChunkSize);
    if (!holdRequests) {
        streamer.requestMissing(
            camCx, camCz, viewRadius, kMaxRequestsPerFrame,
            [&](i32 cx, i32 cz, i32, i32) {
                return !streamer.chunks.contains(chunkKey(cx, cz));
            },
            [&](i32 cx, i32 cz, i32, i32) {
                streamer.chunks.emplace(chunkKey(cx, cz), Chunk {});
                const f32 fade = treeFadeEnd();
                streamer.enqueueBuild(cx, cz, [params, cx, cz, fade] {
                    return scatterProps(params, cx, cz, fade);
                });
            });
    }

    // Evict beyond hysteresis.
    streamer.evictFar(camCx, camCz, viewRadius + 1, [&](Chunk& chunk) {
        if (chunk.resident) {
            if (chunk.total > 0 && chunk.poolOffset != kNoOffset) {
                instancePool.free(chunk.poolOffset, chunk.total);
            }
            instances -= chunk.total;
        }
    });
}

void VegetationSystem::buildPipeline(rhi::Device& device,
                                     ShaderLibrary& shaders) {
    pipeline = { device, device.createPipeline( // U3-7: frees the old one
        { .shader = shaders.get(kTreeShader),
          .vertexBuffers =
              { meshVertexLayout(), // U3-5 (the caster keeps its own
                                    // position+uv layout — sway weights)
                { .stride = sizeof(Instance),
                  .stepMode = rhi::VertexStepMode::Instance,
                  .attributes = { { .location = 4,
                                    .format = rhi::VertexFormat::F32x4,
                                    .offset = offsetof(Instance,
                                                       positionScale) },
                                  { .location = 5,
                                    .format = rhi::VertexFormat::F32x4,
                                    .offset = offsetof(Instance, params) } } } },
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Greater }, // reversed-Z
          .cull = rhi::CullMode::Back }) };
    shaderGeneration = shaders.generation(kTreeShader);
}

void VegetationSystem::buildCasterPipeline(rhi::Device& device,
                                           ShaderLibrary& shaders) {
    casterPipeline = { device, device.createPipeline( // U3-7
        { .shader = shaders.get(kPropCasterShader),
          .vertexBuffers =
              { { .stride = sizeof(MeshVertex),
                  .attributes = { { .location = 0,
                                    .format = rhi::VertexFormat::F32x3,
                                    .offset = offsetof(MeshVertex, position) },
                                  { .location = 2,
                                    .format = rhi::VertexFormat::F32x2,
                                    .offset = offsetof(MeshVertex, uv) } } },
                { .stride = sizeof(Instance),
                  .stepMode = rhi::VertexStepMode::Instance,
                  .attributes = { { .location = 4,
                                    .format = rhi::VertexFormat::F32x4,
                                    .offset = offsetof(Instance,
                                                       positionScale) },
                                  { .location = 5,
                                    .format = rhi::VertexFormat::F32x4,
                                    .offset = offsetof(Instance, params) } } } },
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Less },
          .cull = rhi::CullMode::Back,
          .depthBias = 4.0f,
          .depthBiasSlope = 2.5f }) };
    casterShaderGeneration = shaders.generation(kPropCasterShader);
}

void VegetationSystem::refreshPipeline(rhi::Device& device,
                                       ShaderLibrary& shaders) {
    if (shaders.generation(kTreeShader) != shaderGeneration) {
        buildPipeline(device, shaders);
    }
    if (shaders.generation(kPropCasterShader) != casterShaderGeneration) {
        buildCasterPipeline(device, shaders);
    }
}

void VegetationSystem::draw(rhi::CommandBuffer& cmd,
                            rhi::BindGroupHandle frameBindGroup,
                            rhi::BindGroupHandle shadowBindGroup,
                            u32 variantLimit, const Vec3& cameraPos,
                            bool forceLowDetail, const Frustum* frustum,
                            const std::unordered_set<u64>* occluded) {
    // Frustum verdict per chunk, computed once (the variant-major loops
    // revisit every chunk per variant). Props overhang their chunk: pad XZ
    // by the canopy reach and the top by the tallest scaled tree.
    const auto chunkVisible = [&](u64 key, const Chunk& chunk) {
        if (occluded && occluded->contains(key)) {
            return false; // hidden behind a ridge (ChunkOcclusion)
        }
        if (!frustum) {
            return true;
        }
        const f32 x0 =
            static_cast<f32>(chunkKeyCx(key)) * TerrainSystem::kChunkSize;
        const f32 z0 =
            static_cast<f32>(chunkKeyCz(key)) * TerrainSystem::kChunkSize;
        return frustum->intersectsAabb(
            { x0 - kPropPadXz, chunk.minY - 1.0f, z0 - kPropPadXz },
            { x0 + TerrainSystem::kChunkSize + kPropPadXz,
              chunk.maxY + kPropPadY,
              z0 + TerrainSystem::kChunkSize + kPropPadXz });
    };
    const bool culling = frustum != nullptr || occluded != nullptr;
    std::unordered_map<u64, bool> visible;
    if (culling) {
        visible.reserve(streamer.chunks.size());
        u32 drawnChunks = 0;
        for (const auto& [key, chunk] : streamer.chunks) {
            const bool v = chunk.resident && chunk.total > 0 &&
                           chunkVisible(key, chunk);
            visible.emplace(key, v);
            drawnChunks += v ? 1u : 0u;
        }
        lastDrawn = drawnChunks;
    }
    const auto culled = [&](u64 key) {
        if (!culling) {
            return false;
        }
        const auto it = visible.find(key);
        return it == visible.end() || !it->second;
    };

    cmd.setPipeline(pipeline);
    cmd.setBindGroup(0, frameBindGroup);
    cmd.setBindGroup(1, leafMaskGroup);
    if (shadowBindGroup.id != 0) {
        cmd.setBindGroup(2, shadowBindGroup);
    }
    // Showcase mode: the explicit instances with variant 0's full-detail
    // mesh, nothing else (tool scenes — the streamer holds no chunks).
    if (showcaseCount != 0) {
        const VariantMesh& mesh = variantMeshes[0];
        // The slot's bark group, like the streamed path — without it
        // the builder's specimen showed flat-normal trunks while the
        // forest showed bark.
        if (mesh.albedoGroup.get().id != 0) {
            cmd.setBindGroup(1, mesh.albedoGroup.get());
        }
        cmd.setVertexBuffer(0, mesh.vertexBuffer.get());
        cmd.setIndexBuffer(mesh.indexBuffer.get(), rhi::IndexFormat::U32);
        cmd.setVertexBuffer(1, showcaseInstances.get());
        cmd.drawIndexed(mesh.indexCount, showcaseCount, 0, 0);
        frameIndices += mesh.indexCount * showcaseCount;
        frameHighInstances += showcaseCount;
        return;
    }
    // Canopy LOD pick, per chunk — THREE levels: 320-face lobes
    // near, 80-face twins mid, 20-face ultra beyond lowDetailRadius (and
    // always in mirrored/downsampled passes). Variants without twins
    // (rocks, bushes, authored overrides) always use their main mesh.
    const i32 camCx = chunkCoordOf(cameraPos.x, TerrainSystem::kChunkSize);
    const i32 camCz = chunkCoordOf(cameraPos.z, TerrainSystem::kChunkSize);
    const auto detailLevel = [&](u64 key, u32 v,
                                 const VariantMesh& mesh) -> u32 {
        if (mesh.lowIndexCount == 0) {
            return 0u;
        }
        if (forceLowDetail) {
            return mesh.ultraIndexCount != 0 ? 2u : 1u;
        }
        const i32 cx = chunkKeyCx(key);
        const i32 cz = chunkKeyCz(key);
        const i32 cheb =
            std::max(std::abs(cx - camCx), std::abs(cz - camCz));
        // Plants fade at ~60 m: the tree radii (2/4 chunks) would keep
        // them full-detail everywhere they are visible. Hero mesh in the
        // camera chunk ring only; twins carry the rest.
        const i32 highRadius = v >= kFirstPlant ? 0 : highDetailRadius;
        if (cheb <= highRadius) {
            return 0u;
        }
        if (mesh.ultraIndexCount == 0 || cheb <= lowDetailRadius) {
            return 1u;
        }
        return 2u;
    };
    // Variant-major, split by LOD: bind each mesh level once, then one
    // instanced draw per chunk holding that variant (firstInstance =
    // offset into the chunk's variant-sorted buffer; needs baseInstance,
    // present on 4.6).
    bool albedoBound = false;
    for (u32 v = 0; v < variantLimit; ++v) {
        const VariantMesh& mesh = variantMeshes[v];
        // Textured plants swap group 1 for the variant's albedo (same
        // layout as the leaf-mask atlas); restore the atlas after.
        if (mesh.albedoGroup.get().id != 0) {
            cmd.setBindGroup(1, mesh.albedoGroup.get());
            albedoBound = true;
        } else if (albedoBound) {
            cmd.setBindGroup(1, leafMaskGroup);
            albedoBound = false;
        }
        const u32 levels = mesh.lowIndexCount == 0
                               ? 1u
                               : (mesh.ultraIndexCount == 0 ? 2u : 3u);
        for (u32 level = 0; level < levels; ++level) {
            const rhi::BufferHandle vb =
                level == 0 ? mesh.vertexBuffer.get()
                : level == 1 ? mesh.lowVertexBuffer.get()
                             : mesh.ultraVertexBuffer.get();
            const rhi::BufferHandle ib =
                level == 0 ? mesh.indexBuffer.get()
                : level == 1 ? mesh.lowIndexBuffer.get()
                             : mesh.ultraIndexBuffer.get();
            const u32 indexCount = level == 0 ? mesh.indexCount
                                   : level == 1 ? mesh.lowIndexCount
                                                : mesh.ultraIndexCount;
            bool meshBound = false;
            for (const auto& [key, chunk] : streamer.chunks) {
                if (!chunk.resident || chunk.counts[v] == 0 ||
                    culled(key) || detailLevel(key, v, mesh) != level) {
                    continue;
                }
                if (!meshBound) {
                    cmd.setVertexBuffer(0, vb);
                    cmd.setIndexBuffer(ib, rhi::IndexFormat::U32);
                    meshBound = true;
                }
                cmd.setVertexBuffer(1, instancePool.buffer.get(),
                                    u64(chunk.poolOffset) *
                                        sizeof(Instance));
                cmd.drawIndexed(indexCount, chunk.counts[v], 0,
                                chunk.firstInstance[v]);
                frameIndices += indexCount * chunk.counts[v];
                (level == 0   ? frameHighInstances
                 : level == 1 ? frameLowInstances
                              : frameUltraInstances) += chunk.counts[v];
            }
        }
    }
}

// Every (variant, level) batch must have its own group slot — an
// out-of-range group aliases another variant's command range and the
// prop blinks with the ping-pong (the kFirstDebris+1 postmortem).
static_assert(VegetationSystem::kGroupBase +
                  VegetationSystem::kVariantCount * 3 <=
              GpuOcclusion::kMaxGroups);

void VegetationSystem::collectDrawCandidates(
    vector<GpuOcclusion::Candidate>& out, const Vec3& cameraPos) const {
    if (showcaseCount != 0) {
        return; // showcase renders through the legacy path only
    }
    const i32 camCx = chunkCoordOf(cameraPos.x, TerrainSystem::kChunkSize);
    const i32 camCz = chunkCoordOf(cameraPos.z, TerrainSystem::kChunkSize);
    for (const auto& [key, chunk] : streamer.chunks) {
        if (!chunk.resident || chunk.total == 0 ||
            chunk.poolOffset == kNoOffset) {
            continue;
        }
        const i32 cx = chunkKeyCx(key);
        const i32 cz = chunkKeyCz(key);
        const i32 cheb =
            std::max(std::abs(cx - camCx), std::abs(cz - camCz));
        // Same padded AABB as draw()'s chunkVisible.
        const f32 x0 = static_cast<f32>(cx) * TerrainSystem::kChunkSize;
        const f32 z0 = static_cast<f32>(cz) * TerrainSystem::kChunkSize;
        const Vec3 lo { x0 - kPropPadXz, chunk.minY - 1.0f,
                        z0 - kPropPadXz };
        const Vec3 hi { x0 + TerrainSystem::kChunkSize + kPropPadXz,
                        chunk.maxY + kPropPadY,
                        z0 + TerrainSystem::kChunkSize + kPropPadXz };
        for (u32 v = 0; v < kVariantCount; ++v) {
            if (chunk.counts[v] == 0) {
                continue;
            }
            // Same level pick as draw()'s detailLevel lambda (plants:
            // hero mesh in the camera chunk ring only).
            const VariantMesh& mesh = variantMeshes[v];
            const i32 highRadius =
                v >= kFirstPlant ? 0 : highDetailRadius;
            u32 level = 0;
            if (mesh.lowIndexCount != 0 && cheb > highRadius) {
                level = mesh.ultraIndexCount == 0 || cheb <= lowDetailRadius
                            ? 1u
                            : 2u;
            }
            const u32 indexCount = level == 0   ? mesh.indexCount
                                   : level == 1 ? mesh.lowIndexCount
                                                : mesh.ultraIndexCount;
            out.push_back({ lo, hi, kGroupBase + v * 3 + level, indexCount,
                            0, chunk.counts[v],
                            chunk.poolOffset + chunk.firstInstance[v] });
        }
    }
}

void VegetationSystem::drawIndirect(rhi::CommandBuffer& cmd,
                                    rhi::BindGroupHandle frameBindGroup,
                                    rhi::BindGroupHandle shadowBindGroup,
                                    rhi::BufferHandle commands,
                                    const u32* groupFirst,
                                    const u32* groupCount) {
    cmd.setPipeline(pipeline);
    cmd.setBindGroup(0, frameBindGroup);
    cmd.setBindGroup(1, leafMaskGroup);
    if (shadowBindGroup.id != 0) {
        cmd.setBindGroup(2, shadowBindGroup);
    }
    // One pooled instance buffer for every batch; each command's
    // firstInstance addresses its chunk slice.
    cmd.setVertexBuffer(1, instancePool.buffer.get());
    constexpr u32 kStride = sizeof(rhi::DrawIndexedIndirectCommand);
    bool albedoBound = false;
    for (u32 v = 0; v < kVariantCount; ++v) {
        const VariantMesh& mesh = variantMeshes[v];
        if (mesh.albedoGroup.get().id != 0) {
            cmd.setBindGroup(1, mesh.albedoGroup.get());
            albedoBound = true;
        } else if (albedoBound) {
            cmd.setBindGroup(1, leafMaskGroup);
            albedoBound = false;
        }
        const u32 levels = mesh.lowIndexCount == 0
                               ? 1u
                               : (mesh.ultraIndexCount == 0 ? 2u : 3u);
        for (u32 level = 0; level < levels; ++level) {
            const u32 group = kGroupBase + v * 3 + level;
            if (groupCount[group] == 0) {
                continue;
            }
            cmd.setVertexBuffer(0, level == 0 ? mesh.vertexBuffer.get()
                                   : level == 1
                                       ? mesh.lowVertexBuffer.get()
                                       : mesh.ultraVertexBuffer.get());
            cmd.setIndexBuffer(level == 0   ? mesh.indexBuffer.get()
                               : level == 1 ? mesh.lowIndexBuffer.get()
                                            : mesh.ultraIndexBuffer.get(),
                               rhi::IndexFormat::U32);
            cmd.drawIndexedIndirect(commands,
                                    u64(groupFirst[group]) * kStride,
                                    groupCount[group], kStride);
        }
    }
    // (The per-level instance counters can't be known CPU-side on this
    // path — the panel's dissection belongs to the legacy A/B.)
}

void VegetationSystem::drawDepth(rhi::CommandBuffer& cmd,
                                 rhi::BindGroupHandle frameBindGroup,
                                 rhi::BindGroupHandle casterBindGroup,
                                 const Vec3& cameraPos,
                                 i32 maxChunkDistance,
                                 const Frustum* frustum, bool ultraDetail) {
    const i32 camCx = chunkCoordOf(cameraPos.x, TerrainSystem::kChunkSize);
    const i32 camCz = chunkCoordOf(cameraPos.z, TerrainSystem::kChunkSize);
    cmd.setPipeline(casterPipeline);
    cmd.setBindGroup(0, frameBindGroup);
    cmd.setBindGroup(1, casterBindGroup);
    cmd.setBindGroup(2, leafMaskGroup);
    // Showcase mode: the explicit instances cast with the full-detail
    // mesh — one tree, no LOD/proxy economics needed.
    if (showcaseCount != 0) {
        const VariantMesh& mesh = variantMeshes[0];
        cmd.setVertexBuffer(0, mesh.vertexBuffer.get());
        cmd.setIndexBuffer(mesh.indexBuffer.get(), rhi::IndexFormat::U32);
        cmd.setVertexBuffer(1, showcaseInstances.get());
        cmd.drawIndexed(mesh.indexCount, showcaseCount, 0, 0);
        frameIndices += mesh.indexCount * showcaseCount;
        return;
    }
    for (u32 v = 0; v < kVariantCount; ++v) {
        if (v >= kFirstPlant) {
            continue; // plants cast no shadows (cutout accents — GoT
                      // model; their fill-rate stays out of the cascades)
        }
        bool meshBound = false;
        for (const auto& [key, chunk] : streamer.chunks) {
            if (!chunk.resident || chunk.counts[v] == 0) {
                continue;
            }
            const i32 cx = chunkKeyCx(key);
            const i32 cz = chunkKeyCz(key);
            if (std::max(std::abs(cx - camCx), std::abs(cz - camCz)) >
                maxChunkDistance) {
                continue; // beyond the last shadow cascade
            }
            if (frustum != nullptr) {
                // Same AABB convention as draw() (kPropPad*: canopy
                // overhang in XZ, tallest scaled tree in Y).
                const f32 x0 =
                    static_cast<f32>(cx) * TerrainSystem::kChunkSize;
                const f32 z0 =
                    static_cast<f32>(cz) * TerrainSystem::kChunkSize;
                if (!frustum->intersectsAabb(
                        { x0 - kPropPadXz, chunk.minY - 1.0f,
                          z0 - kPropPadXz },
                        { x0 + TerrainSystem::kChunkSize + kPropPadXz,
                          chunk.maxY + kPropPadY,
                          z0 + TerrainSystem::kChunkSize + kPropPadXz })) {
                    continue; // outside this cascade's ortho volume
                }
            }
            // Casters use the cheapest twin the cascade tolerates: the
            // 80-face lobe throws the same soft shadow as a 320-face one;
            // the far cascades (ultraDetail) prefer the SOLID shadow
            // proxy (metaball blobs — no cards, no cutout), else the
            // 20-face level — their texels are meters wide anyway.
            const VariantMesh& mesh = variantMeshes[v];
            const bool proxy = ultraDetail && mesh.casterIndexCount != 0;
            const bool ultra =
                !proxy && ultraDetail && mesh.ultraIndexCount != 0;
            const bool low = !proxy && !ultra && mesh.lowIndexCount != 0;
            if (!meshBound) {
                cmd.setVertexBuffer(0,
                                    proxy   ? mesh.casterVertexBuffer.get()
                                    : ultra ? mesh.ultraVertexBuffer.get()
                                    : low   ? mesh.lowVertexBuffer.get()
                                            : mesh.vertexBuffer.get());
                cmd.setIndexBuffer(proxy   ? mesh.casterIndexBuffer.get()
                                   : ultra ? mesh.ultraIndexBuffer.get()
                                   : low   ? mesh.lowIndexBuffer.get()
                                           : mesh.indexBuffer.get(),
                                   rhi::IndexFormat::U32);
                meshBound = true;
            }
            const u32 indexCount = proxy   ? mesh.casterIndexCount
                                   : ultra ? mesh.ultraIndexCount
                                   : low   ? mesh.lowIndexCount
                                           : mesh.indexCount;
            cmd.setVertexBuffer(1, instancePool.buffer.get(),
                                u64(chunk.poolOffset) * sizeof(Instance));
            cmd.drawIndexed(indexCount, chunk.counts[v], 0,
                            chunk.firstInstance[v]);
            frameIndices += indexCount * chunk.counts[v];
            (proxy || ultra ? frameUltraInstances : frameLowInstances) +=
                chunk.counts[v];
        }
    }
}

void VegetationSystem::collectGiProps(const Vec3& center, f32 halfSpan,
                                      vector<GiProp>& out,
                                      size_t maxProps) const {
    // Nearest chunks first so the box cap keeps the props that matter.
    struct Near {
        f32 distance;
        const Chunk* chunk;
    };
    vector<Near> near;
    for (const auto& [key, chunk] : streamer.chunks) {
        if (!chunk.resident || chunk.giProps.empty()) {
            continue;
        }
        const f32 cx = (static_cast<f32>(chunkKeyCx(key)) + 0.5f) *
                       TerrainSystem::kChunkSize;
        const f32 cz = (static_cast<f32>(chunkKeyCz(key)) + 0.5f) *
                       TerrainSystem::kChunkSize;
        const f32 distance = glm::max(glm::abs(cx - center.x),
                                      glm::abs(cz - center.z));
        if (distance > halfSpan + TerrainSystem::kChunkSize) {
            continue;
        }
        near.push_back({ distance, &chunk });
    }
    std::sort(near.begin(), near.end(),
              [](const Near& a, const Near& b) {
                  return a.distance < b.distance;
              });
    for (const Near& entry : near) {
        for (const GiProp& prop : entry.chunk->giProps) {
            if (out.size() >= maxProps) {
                return;
            }
            if (glm::max(glm::abs(prop.position.x - center.x),
                         glm::abs(prop.position.z - center.z)) <= halfSpan) {
                out.push_back(prop);
            }
        }
    }
}

} // namespace render
