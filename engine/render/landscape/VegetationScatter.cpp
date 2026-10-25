// The pure-CPU prop scatter (worker-side): masks + scatterProps. Split
// from VegetationSystem.cpp — one TU per trade (scatter / assets /
// streaming / render); the class stays whole in the header. The RNG
// draw order is the contract (VegetationScatterTest).
#include "engine/render/landscape/VegetationSystem.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "engine/core/Hash.hpp"
#include "engine/core/Math.hpp"
#include "engine/render/landscape/PropEncoding.hpp"
#include "engine/render/landscape/TerrainSystem.hpp"

namespace render {

namespace {

// hashU32 / HashRng live in engine/core/Hash.hpp (shared scatter hash family).
using core::hashU32;
using core::HashRng;
using PropFamily = VegetationSystem::PropFamily;

// Bush clumps: medium-frequency blobs so shrubs come in family groups —
// and, crossed with the forest-edge factor, break its isoline into clusters
// instead of tracing it as a string.
f32 bushClumpMask(u32 seed, f32 x, f32 z) {
    const f32 blob = terrain::noise01(seed ^ 0x452821e6u, x / 13.0f,
                                      z / 13.0f);
    return glm::smoothstep(0.52f, 0.66f, blob);
}

// The forest belt thinned by aridity into savanna: the steppe keeps
// isolated trees (~1/5 density), the arid core almost none — the SAME
// dryBand vocabulary as the shader's withered-grass deposit, so the
// trees (and their floor litter) thin exactly where the ground dries.
f32 aridForestAt(const TerrainParams& params, f32 x, f32 z) {
    f32 forest = forestAt(params, x, z);
    forest *= 1.0f -
              0.88f * glm::smoothstep(
                          0.08f, 0.38f,
                          terrain::regionFieldsAt(params, x, z).sandiness);
    return forest;
}

// --- The scatter rules ------------------------------------------------
// Every prop family scatters through ONE loop over a rule table: a
// jittered candidate grid, the terrain probe, the common gates (sea,
// slope, tree line, local water), the family's accept hook, then the
// placement draws. The per-candidate RNG draw ORDER is the contract
// (VegetationScatterTest golden): position (2 draws), whatever the
// accept hook draws, then variant (when drawn), scale, yaw,
// [preAccept], tint, phase. The gates are pure (no draw), so their
// order against the hook never moves a draw.

// The chunk the loop scatters into (shared by the hooks).
struct ChunkScatter {
    const TerrainParams& params;
    const terrain::WaterBodiesSubset& water;
    VegetationSystem::VariantBuckets& buckets;
    f32 treeFadeEnd;
};

// One candidate after the terrain probe.
struct Candidate {
    f32 x;
    f32 z;
    f32 h;
    Vec3 n;
    f32 slope; // 1 - n.y
};

// What the loop places for an accepted candidate. Pre-filled from the
// rule's columns; the accept hook may override it (the tree rule's
// altitude bands pick their own variant range, scale and even family).
struct Placement {
    u32 firstVariant;
    u32 variantCount;
    f32 y;
    f32 scaleMin;
    // Width of the scale range, carried as such (not as a max): the
    // span is what the scale draw multiplies, and a literal span does
    // not round-trip through (min + span) - min in float.
    f32 scaleSpan;
    f32 fadeEnd;
    bool textured;
    bool rigid;
};

// How the variant slot is chosen: one RNG draw across the family's
// slots, or fixed by the accept hook (habitat-picked species).
enum class VariantPick : u8 { Draw, Fixed };

constexpr f32 kNoSeaGate = -std::numeric_limits<f32>::infinity();
constexpr f32 kNoSlopeGate = std::numeric_limits<f32>::infinity();

struct ScatterRule {
    u32 salt;
    f32 spacing;     // meters between candidates
    f32 seaMargin;   // reject h < seaLevel + margin (kNoSeaGate = none)
    f32 slopeMax;    // reject slope > max (kNoSlopeGate = none)
    f32 waterMargin; // underLocalWater margin (m)
    bool belowTreeLine; // reject h >= treeLine
    PropFamily family;
    VariantPick pick;
    f32 scaleMin;
    f32 scaleSpan;
    f32 ySink;   // seat below the terrain point (m)
    f32 fadeEnd; // reach (m); the trees take the live ring value
    bool textured;
    bool rigid;
    // Plants/mass: acceptance by species slot (tall grass, fern,
    // dandelion, shrub) — each density tunes independently.
    f32 speciesAccept[4];
    // Draws the rule's acceptance; may rewrite `out`.
    bool (*accept)(const ChunkScatter&, const ScatterRule&, const Candidate&,
                   HashRng&, Placement& out);
    // Optional veto AFTER the variant/scale/yaw draws and BEFORE the
    // tint/phase draws, for footprints that need their yaw first; may
    // reseat `y`.
    bool (*preAccept)(const ChunkScatter&, const Candidate&, u32 variant,
                      f32 scale, f32 yaw, f32& y);
};

// Trees: forest belts on gentle grassy mid-altitude ground.
bool acceptTree(const ChunkScatter& chunk, const ScatterRule&,
                const Candidate& c, HashRng& rng, Placement& out) {
    const f32 forest = aridForestAt(chunk.params, c.x, c.z);
    // Density halving done on the acceptance rather than the spacing so
    // the grid keeps its resolution (spacing x sqrt(2) would truncate).
    if (forest < 0.05f || rng.next() >= forest * 0.475f) {
        return false;
    }
    // ALTITUDE BANDS, fractions of the tree line: broadleaf country low,
    // a mixed belt, conifers up to the line (the forest thins and
    // stunts over its last stretch — a real treeline dissolves), then a
    // krummholz band of bushes just past it. Grass and rock own
    // everything above.
    const f32 h01 = c.h / terrain::treeLine(chunk.params);
    if (h01 >= 1.0f) {
        if (h01 < 1.15f && rng.next() < 0.35f) {
            // Krummholz: a bush placed by the tree rule.
            const auto& bushes =
                VegetationSystem::familyTraits(PropFamily::Bush);
            out.firstVariant = bushes.first;
            out.variantCount = bushes.count;
            out.y = c.h - 0.05f;
            out.scaleMin = 0.8f;
            out.scaleSpan = 1.5f - 0.8f;
            out.fadeEnd = 660.0f;
            return true;
        }
        return false;
    }
    const f32 lineFade = 1.0f - glm::smoothstep(0.82f, 1.0f, h01);
    if (rng.next() >= lineFade) {
        return false;
    }
    out.firstVariant = 0;
    out.variantCount = VegetationSystem::kBroadleafVariants;
    if (h01 >= 0.7f) { // conifer belt
        out.firstVariant = VegetationSystem::kBroadleafVariants;
        out.variantCount = VegetationSystem::kTreeVariants -
                           VegetationSystem::kBroadleafVariants;
    } else if (h01 >= 0.45f) { // mixed belt
        out.variantCount = VegetationSystem::kTreeVariants;
    }
    const f32 stunt = 0.55f + 0.45f * lineFade;
    const f32 scaleMin = VegetationSystem::kTreeScaleMin * stunt;
    const f32 scaleMax = VegetationSystem::kTreeScaleMax * stunt;
    out.scaleMin = scaleMin;
    out.scaleSpan = scaleMax - scaleMin;
    out.fadeEnd = chunk.treeFadeEnd;
    return true;
}

// Rocks: sparse everywhere, denser on rocky/alpine ground.
bool acceptRock(const ChunkScatter& chunk, const ScatterRule&,
                const Candidate& c, HashRng& rng, Placement&) {
    const auto weights =
        terrain::materialWeightsAt(chunk.params, c.x, c.z, c.h, c.n);
    // Boulders belong to rocky and alpine ground first, meadows get the
    // occasional loner.
    const f32 chance = 0.08f + 0.42f * weights.rock + 0.32f * weights.snow;
    return !(rng.next() >= chance);
}

// Pebbles: the ground-clutter read (docs/PAYSAGE.md §1.7) — the SAME
// boulder scans at centimeter scale: dense along the grass/rock blend
// band (cross-scatter — stones spill into the fringe grass), sparse
// elsewhere on rocky or bare-dirt ground.
bool acceptPebble(const ChunkScatter& chunk, const ScatterRule&,
                  const Candidate& c, HashRng& rng, Placement&) {
    const auto weights =
        terrain::materialWeightsAt(chunk.params, c.x, c.z, c.h, c.n);
    // The blend band peaks at 50/50 grass/rock; the stony (2) and
    // bare-dirt (3) ground variants carry extra loners. Pure meadow
    // keeps only a thin floor — the plants own the grassy read, pebbles
    // own the transition.
    const f32 band = 4.0f * weights.grass * weights.rock;
    f32 chance = 0.02f + 0.45f * band + 0.10f * weights.rock;
    const u32 zone = terrain::grassZoneAt(c.x, c.z).variantA;
    if (zone == 2 || zone == 3) {
        chance *= 1.6f;
    }
    return !(rng.next() >= chance);
}

// Forest-floor debris (stumps, fallen trunks): forest interior only —
// debris is what a forest floor leaves behind; a savanna floor keeps no
// litter.
bool acceptDebris(const ChunkScatter& chunk, const ScatterRule&,
                  const Candidate& c, HashRng& rng, Placement&) {
    const f32 forest = aridForestAt(chunk.params, c.x, c.z);
    return !(forest < 0.55f || rng.next() >= 0.16f * forest);
}

// The fallen trunk needs its yaw and footprint BEFORE acceptance (tree
// clearance, ground probes).
bool preAcceptDebris(const ChunkScatter& chunk, const Candidate& c,
                     u32 variant, f32 scale, f32 yaw, f32& y) {
    const auto& debris = VegetationSystem::familyTraits(PropFamily::Debris);
    const bool log = variant == debris.first + 1;
    // tree.vert maps local X to (cos yaw, 0, sin yaw).
    const f32 halfLen = log ? 1.1f * scale : 0.0f;
    const f32 dx = std::cos(yaw) * halfLen;
    const f32 dz = std::sin(yaw) * halfLen;
    // Trees landed first (rule order): reject debris lying into a
    // standing trunk — bark-on-bark interpenetration z-fights.
    // Segment-to-base distance in XZ; same-chunk trees only (a
    // neighbour's tree can still graze a log near the border — rare,
    // accepted).
    const auto& trees = VegetationSystem::familyTraits(PropFamily::Tree);
    for (u32 tv = trees.first; tv < trees.end(); ++tv) {
        for (const VegetationSystem::Instance& tree : chunk.buckets[tv]) {
            const f32 tx = tree.positionScale.x - c.x;
            const f32 tz = tree.positionScale.z - c.z;
            const f32 along =
                halfLen > 0.0f
                    ? glm::clamp(tx * (dx / halfLen) + tz * (dz / halfLen),
                                 -halfLen, halfLen)
                    : 0.0f;
            const f32 ox =
                tx - (halfLen > 0.0f ? along * (dx / halfLen) : 0.0f);
            const f32 oz =
                tz - (halfLen > 0.0f ? along * (dz / halfLen) : 0.0f);
            // Bark radius grows with the tree scale (the collision box
            // uses 0.28 x scale) + log girth.
            const f32 r = 0.7f + 0.3f * tree.positionScale.w;
            if (ox * ox + oz * oz < r * r) {
                return false;
            }
        }
    }
    if (log) {
        // Props carry yaw only (no slope alignment) — probe the ground
        // along the axis, reject ground a straight log cannot lie on,
        // seat on the CREST (a half-buried log leaves a grazing surface
        // that z-fights the terrain).
        const f32 hA = terrain::height(chunk.params, c.x + dx, c.z + dz);
        const f32 hB = terrain::height(chunk.params, c.x - dx, c.z - dz);
        const f32 hi = glm::max(c.h, glm::max(hA, hB));
        const f32 lo = glm::min(c.h, glm::min(hA, hB));
        if (hi - lo > 0.30f * scale) {
            return false;
        }
        y = hi - 0.05f * scale;
    }
    return true;
}

// Plants + mass tier: the photoreal understory (docs/PAYSAGE.md §1.7
// palier 2). Textured cutout scans picked by HABITAT: fern inside
// forests, shrub on their edges, dandelion in open meadow, tall grass
// anywhere grassy. Two rules over the SAME habitat/colony/zone logic:
// the hero plants (sparse, focal), then the mass tier — cheap clones at
// high density and shorter reach, the carpet the heroes sit on. Same
// colony field, so carpets and heroes agree.
bool acceptPlant(const ChunkScatter& chunk, const ScatterRule& rule,
                 const Candidate& c, HashRng& rng, Placement& out) {
    if (terrain::materialWeightsAt(chunk.params, c.x, c.z, c.h, c.n).grass <
        0.55f) {
        return false;
    }
    const f32 forest = forestAt(chunk.params, c.x, c.z);
    // Habitat pick.
    u32 species = 0; // tall grass
    if (forest > 0.4f) {
        species = 1; // fern
    } else if (forest > 0.15f) {
        species = 3; // shrub
    } else if (rng.next() < 0.35f) {
        species = 2; // dandelion
    }
    f32 accept = rule.speciesAccept[species];
    // COLONY noise (~12 m, per species): nature grows in patches —
    // dense hearts, empty clearings.
    const f32 colony = glm::smoothstep(
        0.45f, 0.75f,
        terrain::noise01(chunk.params.seed ^ (0xa11c03du + species),
                         c.x * 0.08f, c.z * 0.08f));
    accept *= 2.0f * colony;
    // Worn/dirt ground variants carry fewer plants.
    const u32 zone = terrain::grassZoneAt(c.x, c.z).variantA;
    if (zone == 1 || zone == 3) {
        accept *= 0.5f;
    }
    if (rng.next() >= accept) {
        return false;
    }
    out.firstVariant = VegetationSystem::familyTraits(rule.family).first +
                       species;
    return true;
}

// Bushes: grassy ground, biased toward forest edges.
bool acceptBush(const ChunkScatter& chunk, const ScatterRule&,
                const Candidate& c, HashRng& rng, Placement&) {
    // 0.5 keeps bushes alive in the alpine biome (grassPresence 0.6);
    // arid scrub stays excluded.
    if (terrain::materialWeightsAt(chunk.params, c.x, c.z, c.h, c.n).grass <
        0.5f) {
        return false;
    }
    // Clumps gate everything (bushes come in family groups); the
    // forest-edge factor then biases WHICH clumps are lush, without
    // tracing the treeline as a string.
    const f32 clump = bushClumpMask(chunk.params.seed, c.x, c.z);
    if (clump < 0.05f) {
        return false;
    }
    const f32 forest = forestAt(chunk.params, c.x, c.z);
    const f32 edge = forest * (1.0f - forest) * 4.0f;
    return !(rng.next() >= clump * (0.35f + 0.65f * edge));
}

// Rule ORDER is part of the contract: a family's bucket keeps its push
// order (the krummholz bushes land before the bush rule's own), and the
// debris veto reads the trees already placed.
constexpr ScatterRule kRules[] = {
    // Trees: realistic scale (x8 height against the player), so 2x the
    // candidate spacing = 1/4 the density — giant forests, not hedges
    // of them. Sunk so leaning trunks never float on slopes. Scale,
    // variant range and reach come from the altitude bands (acceptTree).
    { .salt = 0x2545f491u,
      .spacing = 8.0f,
      .seaMargin = 3.0f,
      .slopeMax = 0.3f,
      .waterMargin = 1.0f,
      .belowTreeLine = false,
      .family = PropFamily::Tree,
      .pick = VariantPick::Draw,
      .scaleMin = VegetationSystem::kTreeScaleMin,
      .scaleSpan = VegetationSystem::kTreeScaleMax -
                   VegetationSystem::kTreeScaleMin,
      .ySink = 0.9f,
      .fadeEnd = 880.0f,
      .textured = false,
      .rigid = false,
      .speciesAccept = {},
      .accept = acceptTree,
      .preAccept = nullptr },
    // Rocks: shoreline boulders stay (0.5 m), no cliff faces; the
    // fresh-water test keeps them (and their colliders) out of lake
    // sheets and rivers. Photogrammetry albedo, rigid; 25% shorter
    // reach than trees.
    { .salt = 0x8f14ab5du,
      .spacing = 7.5f,
      .seaMargin = 0.5f,
      .slopeMax = 0.55f,
      .waterMargin = 0.3f,
      .belowTreeLine = false,
      .family = PropFamily::Rock,
      .pick = VariantPick::Draw,
      .scaleMin = 0.5f,
      .scaleSpan = 2.0f - 0.5f,
      .ySink = 0.10f,
      .fadeEnd = 660.0f,
      .textured = true,
      .rigid = true,
      .speciesAccept = {},
      .accept = acceptRock,
      .preAccept = nullptr },
    // Pebbles: decimated scans, rigid; short reach — a near-field read.
    { .salt = 0x3e5a91c7u,
      .spacing = 1.8f,
      .seaMargin = 0.5f,
      .slopeMax = 0.6f,
      .waterMargin = 0.05f,
      .belowTreeLine = false,
      .family = PropFamily::Pebble,
      .pick = VariantPick::Draw,
      .scaleMin = 0.06f,
      .scaleSpan = 0.25f - 0.06f,
      .ySink = 0.02f,
      .fadeEnd = 90.0f,
      .textured = true,
      .rigid = true,
      .speciesAccept = {},
      .accept = acceptPebble,
      .preAccept = nullptr },
    // Forest-floor debris: scanned, rigid; the long reach keeps it out
    // of the distance thinning (structural, like the boulders).
    { .salt = 0x7a3f19e5u,
      .spacing = 14.0f,
      .seaMargin = 3.0f,
      .slopeMax = 0.35f,
      .waterMargin = 0.5f,
      .belowTreeLine = true,
      .family = PropFamily::Debris,
      .pick = VariantPick::Draw,
      .scaleMin = 1.2f,
      .scaleSpan = 2.6f - 1.2f,
      .ySink = 0.06f,
      .fadeEnd = 300.0f,
      .textured = true,
      .rigid = true,
      .speciesAccept = {},
      .accept = acceptDebris,
      .preAccept = preAcceptDebris },
    // Hero plants: textured cutouts that sway; density leans on the
    // shader's distance ramp (thin key) — the near field is dense, the
    // far field a deterministic subset.
    { .salt = 0x5c17ba31u,
      .spacing = 3.0f,
      .seaMargin = 0.5f,
      .slopeMax = 0.4f,
      .waterMargin = 0.1f,
      .belowTreeLine = true,
      .family = PropFamily::Plant,
      .pick = VariantPick::Fixed,
      .scaleMin = 0.9f,
      .scaleSpan = 0.6f,
      .ySink = 0.03f,
      .fadeEnd = 60.0f,
      .textured = true,
      .rigid = false,
      .speciesAccept = { 0.18f, 0.32f, 0.20f, 0.14f },
      .accept = acceptPlant,
      .preAccept = nullptr },
    // Mass tier: the same species as cheap clones, denser and shorter.
    { .salt = 0x9dd23b71u,
      .spacing = 2.0f,
      .seaMargin = 0.5f,
      .slopeMax = 0.4f,
      .waterMargin = 0.1f,
      .belowTreeLine = true,
      .family = PropFamily::Mass,
      .pick = VariantPick::Fixed,
      .scaleMin = 0.7f,
      .scaleSpan = 0.6f,
      .ySink = 0.03f,
      .fadeEnd = 35.0f,
      .textured = true,
      .rigid = false,
      .speciesAccept = { 0.35f, 0.55f, 0.35f, 0.25f },
      .accept = acceptPlant,
      .preAccept = nullptr },
    // Bushes: no sea or slope gate (the grass weight gates them); small
    // silhouettes, rock reach.
    { .salt = 0xc1d1f0adu,
      .spacing = 5.0f,
      .seaMargin = kNoSeaGate,
      .slopeMax = kNoSlopeGate,
      .waterMargin = 0.3f,
      .belowTreeLine = false,
      .family = PropFamily::Bush,
      .pick = VariantPick::Draw,
      .scaleMin = 0.7f,
      .scaleSpan = 1.3f - 0.7f,
      .ySink = 0.05f,
      .fadeEnd = 660.0f,
      .textured = false,
      .rigid = false,
      .speciesAccept = {},
      .accept = acceptBush,
      .preAccept = nullptr },
};

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

f32 forestAt(const TerrainParams& params, f32 x, f32 z) {
    const f32 density = terrain::regionFieldsAt(params, x, z).forestDensity;
    return glm::clamp(forestMask(params.seed, x, z) * density, 0.0f, 1.0f);
}

VegetationSystem::VariantBuckets scatterProps(const TerrainParams& params,
                                              i32 cx, i32 cz,
                                              f32 treeFadeEnd) {
    const f32 originX = static_cast<f32>(cx) * TerrainSystem::kChunkSize;
    const f32 originZ = static_cast<f32>(cz) * TerrainSystem::kChunkSize;
    VegetationSystem::VariantBuckets buckets;

    // Chunk-scoped water subset: every candidate below queries only the
    // bodies whose bounds reach this chunk (±8 m slack over the jittered
    // grids) instead of all resident lakes/rivers — bit-identical
    // (WaterBodies.hpp).
    const terrain::WaterBodiesSubset chunkWater =
        params.water
            ? terrain::waterBodiesInRect(
                  *params.water, originX - 8.0f, originZ - 8.0f,
                  originX + TerrainSystem::kChunkSize + 8.0f,
                  originZ + TerrainSystem::kChunkSize + 8.0f)
            : terrain::WaterBodiesSubset {};
    const ChunkScatter chunk { params, chunkWater, buckets, treeFadeEnd };

    const auto candidateRng = [&](u32 salt, u32 index) {
        return HashRng { hashU32(params.seed ^ salt) ^
                         hashU32(static_cast<u32>(cx * 83492791 ^
                                                  cz * 297121507) ^
                                 index) };
    };

    for (const ScatterRule& rule : kRules) {
        const VegetationSystem::PropFamilyTraits& family =
            VegetationSystem::familyTraits(rule.family);
        const u32 perSide =
            static_cast<u32>(TerrainSystem::kChunkSize / rule.spacing);
        for (u32 i = 0; i < perSide * perSide; ++i) {
            HashRng rng = candidateRng(rule.salt, i);
            Candidate c;
            c.x = originX + (static_cast<f32>(i % perSide) + rng.next()) *
                                rule.spacing;
            c.z = originZ + (static_cast<f32>(i / perSide) + rng.next()) *
                                rule.spacing;
            c.h = terrain::height(params, c.x, c.z);
            c.n = terrain::normal(params, c.x, c.z);
            c.slope = 1.0f - c.n.y;
            if (c.h < params.seaLevel + rule.seaMargin ||
                c.slope > rule.slopeMax ||
                (rule.belowTreeLine && c.h >= terrain::treeLine(params)) ||
                terrain::underLocalWater(params, chunkWater, c.x, c.z, c.h,
                                         rule.waterMargin)) {
                continue;
            }
            Placement p { family.first,   family.count,  c.h - rule.ySink,
                          rule.scaleMin,  rule.scaleSpan, rule.fadeEnd,
                          rule.textured,  rule.rigid };
            if (!rule.accept(chunk, rule, c, rng, p)) {
                continue;
            }
            // Draw order is a CONTRACT: variant, scale, yaw, tint,
            // phase — reordering reseeds every prop in the world.
            u32 variant = p.firstVariant;
            if (rule.pick == VariantPick::Draw) {
                variant += glm::min(
                    static_cast<u32>(rng.next() *
                                     static_cast<f32>(p.variantCount)),
                    p.variantCount - 1);
            }
            const f32 scale = p.scaleMin + rng.next() * p.scaleSpan;
            const f32 yaw = rng.next() * core::kTau;
            if (rule.preAccept != nullptr &&
                !rule.preAccept(chunk, c, variant, scale, yaw, p.y)) {
                continue;
            }
            const f32 tint = rng.next();
            const f32 phase = rng.next() * core::kTau;
            buckets[variant].push_back({
                .positionScale = { c.x, p.y, c.z, scale },
                .params = { yaw,
                            tint, // tint jitter / thin key
                            p.rigid ? propenc::rigidPhase(phase) : phase,
                            p.textured ? propenc::texturedFade(p.fadeEnd)
                                       : p.fadeEnd },
            });
        }
    }
    return buckets;
}

} // namespace render
