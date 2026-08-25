// The pure-CPU prop scatter (worker-side): masks + scatterProps. Split
// from VegetationSystem.cpp — one TU per trade (scatter / assets /
// streaming / render); the class stays whole in the header. The RNG
// draw order is the contract (VegetationScatterTest).
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
            f32 forest = forestMask(params.seed, x, z);
            // Aridity thins the forest into savanna: the steppe keeps
            // isolated trees (~1/5 density), the arid core almost none —
            // the SAME dryBand vocabulary as the shader's withered-grass
            // deposit, so the trees thin exactly where the ground dries.
            forest *= 1.0f -
                      0.88f * glm::smoothstep(
                                  0.08f, 0.38f,
                                  terrain::regionFieldsAt(params, x, z)
                                      .sandiness);
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
            // leaves behind. Same aridity thinning as the trees: a
            // savanna floor keeps no forest litter.
            f32 forest = forestMask(params.seed, x, z);
            forest *= 1.0f -
                      0.88f * glm::smoothstep(
                                  0.08f, 0.38f,
                                  terrain::regionFieldsAt(params, x, z)
                                      .sandiness);
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

    // --- Plants + mass tier: the photoreal understory ------------------------
    // (docs/GRASS-REDO.md palier 2.) Textured cutout scans picked by
    // HABITAT: fern inside forests, shrub on their edges, dandelion in
    // open meadow, tall grass anywhere grassy. The NEGATIVE fade lane
    // flags "textured" to tree.vert/frag (uv = texture coords, height
    // sway); reach stays short — a near-field read like the pebbles.
    // Two passes over the SAME habitat/colony/zone rules: the hero
    // plants (sparse, focal), then the mass tier — cheap clones (~200
    // tris) at high density and shorter reach, the carpet the heroes sit
    // on. Same colony field, so carpets and heroes agree. Per-candidate
    // draw order is the RNG contract (VegetationScatterTest).
    struct PlantTier {
        u32 salt;
        f32 spacing;
        f32 fade;
        u32 firstVariant;
        f32 scaleMin;
        // Acceptance by species slot (tall grass, fern, dandelion,
        // shrub) — each density tunes independently.
        f32 accept[4];
    };
    // Hero density leans on the shader's distance ramp (thin key): the
    // near field is dense, the far field a deterministic subset.
    constexpr PlantTier kPlantTiers[] = {
        { 0x5c17ba31u, 3.0f, 60.0f, VegetationSystem::kFirstPlant, 0.9f,
          { 0.18f, 0.32f, 0.20f, 0.14f } },
        { 0x9dd23b71u, 2.0f, 35.0f, VegetationSystem::kFirstMass, 0.7f,
          { 0.35f, 0.55f, 0.35f, 0.25f } },
    };
    for (const PlantTier& tier : kPlantTiers) {
        const u32 perSide =
            static_cast<u32>(TerrainSystem::kChunkSize / tier.spacing);
        for (u32 i = 0; i < perSide * perSide; ++i) {
            HashRng rng = candidateRng(tier.salt, i);
            const f32 x = originX + (static_cast<f32>(i % perSide) +
                                     rng.next()) *
                                        tier.spacing;
            const f32 z = originZ + (static_cast<f32>(i / perSide) +
                                     rng.next()) *
                                        tier.spacing;
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
            // Habitat pick.
            u32 species = 0; // tall grass
            if (forest > 0.4f) {
                species = 1; // fern
            } else if (forest > 0.15f) {
                species = 3; // shrub
            } else if (rng.next() < 0.35f) {
                species = 2; // dandelion
            }
            f32 accept = tier.accept[species];
            // COLONY noise (~12 m, per species): nature grows in
            // patches — dense hearts, empty clearings.
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
            buckets[tier.firstVariant + species].push_back({
                .positionScale = { x, h - 0.03f, z,
                                   tier.scaleMin + rng.next() * 0.6f },
                .params = { rng.next() * 6.2831853f, rng.next(),
                            rng.next() * 6.2831853f,
                            -tier.fade }, // negative = textured cutout
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


} // namespace render
