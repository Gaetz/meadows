#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"
#include "engine/terrain/BiomeMap.hpp"      // render::BiomeParams / BiomeSet
#include "engine/terrain/HeightPatches.hpp" // render::HeightPatch / HeightPatches
#include "engine/terrain/TerrainBase.hpp"   // render::TerrainRegion / TerrainBase
#include "engine/terrain/WaterBodies.hpp"   // render::WaterBodies

namespace render {

// Authored terrain overrides: per-chunk DELTA grids added
// on top of the procedural base — final height = noise + bilinear(delta).
// The world layer builds this from TerrainPatchForm records + `.ter`
// assets (the engine never sees Forms — docs/HORIZONTAL-PASS.md);
// sculpting edits grids then publishes a NEW immutable instance, so
// workers holding the old pointer stay race-free.
// HeightPatch / HeightPatches live in engine/terrain/HeightPatches.hpp
// (a headless home) so the world layer can build them without depending on
// engine/render/. They still ride inside TerrainParams below.

// Default snow altitude (meters) — shared by TerrainParams::snowLine and
// every CPU consumer that needs a pre-tuning value. The live value comes
// from tuning and MUST reach shader and CPU rules in lockstep.
constexpr f32 kSnowLine = 165.0f;

// Terrain generation parameters. One plain struct on purpose: when landscape
// population becomes moddable it converts into a reflected Form patched
// through the §5 plugin system (precedent: StatsTuningForm), and this stays a
// local change.
struct TerrainParams {
    u32 seed { 1337 };

    // Authored overrides — SHARED ownership (postmortem: this was a raw
    // observer with a "the scene keeps it alive" contract, and the scene
    // broke it at teardown — quitting while scatter jobs held copied
    // TerrainParams read a freed HeightPatches and crashed). Every copy,
    // worker-held ones included, keeps the data alive by itself.
    // Null = pure procedural, bit-identical to the patch-free behavior (the
    // non-regression contract). Riding inside TerrainParams means every
    // consumer (workers included) is patched without a signature change.
    sptr<const HeightPatches> patches;

    // Baked base regions (generated terrain): inside a region the height is
    // bicubic(grid) + detail noise instead of the procedural noise below.
    // Overlapping regions (sandbox tiles share their margin ring) blend by
    // edge weight; where total coverage fades out, the procedural fallback
    // blends back in. Same shared-ownership and
    // publish-new-immutable-instance contract as `patches`; null = pure
    // procedural, bit-identical. Sculpt deltas apply on top in every case
    // (layers never flatten).
    sptr<const TerrainBase> base;

    // Sandbox world identity (engine/terrain/SandboxTerrain.hpp). When
    // set, the fallback outside baked regions is the analytic S1 macro
    // instead of the legacy demo noise — far silhouettes then agree with
    // the tiles the streamer bakes. Null = legacy noise (bit-identical).
    sptr<const struct SandboxTerrain> sandbox;

    // Biome table (+ optional painted index map). Null or id 0 = the
    // neutral biome: every rule below behaves exactly as without biomes.
    sptr<const BiomeSet> biomes;

    // Local water (lakes/rivers) for the SCATTER exclusion rules: trees
    // and grass must not grow under an altitude lake the sea-level rule
    // cannot see. Same shared-immutable contract as the layers above;
    // null = sea-only rules (bit-identical legacy).
    sptr<const WaterBodies> water;

    // Bumped whenever the terrain CONTENT changes without the seed
    // moving (tile publishes, mode switches): consumers with their own
    // baked caches (FarTerrain, pool map) compare it to invalidate.
    // Never read by height() — purely a staleness signal.
    u64 contentStamp { 0 };

    // Rolling hills: FBM value noise.
    f32 hillWavelength { 500.0f }; // meters per base octave
    f32 hillAmplitude { 75.0f };
    i32 octaves { 5 };
    f32 lacunarity { 2.0f };
    f32 gain { 0.5f };

    // Ridged mountains, gated by a low-frequency mask so ranges rise in
    // some regions and leave plains elsewhere.
    f32 mountainWavelength { 2000.0f };
    f32 mountainAmplitude { 270.0f };
    f32 mountainMaskLow { 0.45f };  // mask noise below this -> plains
    f32 mountainMaskHigh { 0.75f }; // above this -> full mountains

    f32 seaLevel { kDefaultSeaLevel };
    // Snow altitude for the CPU material rules — MUST match the tuning
    // value the shader gets (uTerrainInfo.y), or footsteps/scatter and
    // pixels disagree about what is snow.
    f32 snowLine { kSnowLine };
    // Tree line as a fraction of the snow line (scatter + far fringe).
    f32 treeLineFactor { 0.82f };
};

namespace terrain {

// Pure and deterministic (integer-hash value noise — no library RNG, no
// state): the same params and coordinates give bit-identical heights on
// every call, chunk, and platform. This is what makes chunk borders match
// with zero stitching.
f32 height(const TerrainParams& params, f32 x, f32 z);

// Central differences of height(); unit length. Analytic (function-derived,
// never mesh-derived) so normals are seamless across chunk borders and LODs.
Vec3 normal(const TerrainParams& params, f32 x, f32 z, f32 step = 0.5f);

// Height for a MESH vertex sampled `spacing` meters apart: identical to
// height() at fine spacing; at coarse LODs, vertices over a baked river
// channel (flow mask) take the MIN over their support — carved beds stay
// open at distance instead of being bridged shut by the decimation
// (which drowned the water surface under the coarse triangles).
f32 meshHeight(const TerrainParams& params, f32 x, f32 z, f32 spacing);

// Raw smooth value noise in [0,1] — the building block, exposed for scatter
// masks (grass patches, forest belts) so they share the terrain's hash.
f32 noise01(u32 seed, f32 x, f32 z);

// Samples the PROCEDURAL base (no baked regions, no sculpt deltas) into an
// absolute-height region grid. Debug/bootstrap path for the baked-base
// layer: the result, installed as `params.base`, reproduces the procedural
// terrain within bicubic-resample tolerance. detailAmplitude is left at 0
// because the procedural base already contains its own high frequencies.
TerrainRegion bakeProceduralRegion(const TerrainParams& params, f32 originX,
                                   f32 originZ, f32 sizeMeters,
                                   f32 texelSize);

// CPU mirror of terrain.frag's splat weights (minus the texture-driven
// border wander): ONE definition of "what grows where" shared by every
// scatter rule. Keep in sync with the shader when tuning.
struct MaterialWeights {
    f32 grass { 0.0f };
    f32 rock { 0.0f };
    f32 snow { 0.0f };
    f32 sand { 0.0f };
    // Bare stratified cliff on the steepest exposed faces — driven by
    // the baked rockExposure mask, so it only appears on baked regions.
    f32 cliff { 0.0f };
};

// The baked rock-exposure mask over (x, z) in [0, 1]; 0 without a baked
// region (legacy/story terrain shows no cliff material).
f32 rockExposureAt(const TerrainParams& params, f32 x, f32 z);

// Upper tree limit (meters), scaled with the active snow line so
// forests climb sandbox mountains: story 165 -> ~135 (the historical
// treeline), sandbox 950 -> ~780. Shared by the near scatter and the
// FarTerrain fringe so the two can never disagree.
inline f32 treeLine(const TerrainParams& params) {
    return params.snowLine * params.treeLineFactor;
}

// True where LOCAL water (params.water: lakes, rivers) covers ground at
// height `h`; `margin` meters above the waterline still count as wet
// (shore band for scatter). Always false with no water set — the sea
// stays each rule's own seaLevel check.
bool underLocalWater(const TerrainParams& params, f32 x, f32 z, f32 h,
                     f32 margin);
MaterialWeights materialWeights(const TerrainParams& params, f32 height,
                                const Vec3& normal);

// The biome over a point: the baked region's biome mask when covered,
// else the painted index map, else neutral. Also the CLIMATE seam —
// gameplay reads temperature/wetness from the returned params through a
// scene-provided callback (HeightFn pattern, §2.10).
const BiomeParams& biomeAt(const TerrainParams& params, f32 x, f32 z);

// Position-aware weights: materialWeights with the biome's character
// applied (snow line shift, rockiness, sand band, grass presence, baked
// beach mask). With no biome data this returns exactly materialWeights —
// existing scatter masks stay unmoved.
MaterialWeights materialWeightsAt(const TerrainParams& params, f32 x,
                                  f32 z, f32 height, const Vec3& normal);

// One sample of the low-frequency region fields — biome attributes
// resolved to CONTINUOUS values (ids never blend; resolving
// id -> attributes here is what makes the shade map's bilinear filtering
// legitimate) plus the baked wetness/beach masks. This is the CHEAP part,
// on the scatter/footstep hot path.
struct RegionFields {
    f32 wetness { 0.0f };
    f32 rockiness { 0.0f };
    f32 snowLineOffset { 0.0f }; // meters
    f32 sandiness { 0.0f };
    f32 beach { 0.0f };
    f32 grassPresence { 1.0f }; // scatter-only (not a shader input)
    f32 temperature { 0.0f };   // climate (tint composition)
    f32 biomeWetness { 0.0f };  // biome character (≠ baked water mask)
};
RegionFields regionFieldsAt(const TerrainParams& params, f32 x, f32 z);

// Fields + the composed macro tint (fbm — DELIBERATELY not on the weight
// mirrors' hot path). THE single source for the TerrainShadeMap bake, the
// grass root-albedo corners and the GI albedo tile.
struct RegionShading {
    RegionFields fields;
    Vec3 tint { 1.0f };
};
RegionShading regionShadingAt(const TerrainParams& params, f32 x, f32 z);

// Grass-family ground variant zoning — the CPU mirror of
// terrain_zones.glsl (same hash, lattice, jitter, coloring): jittered-grid
// Voronoi cells whose 2x2 base coloring guarantees two orthogonal
// neighbors never share a variant. Seed-independent world pattern (like
// borderWander). Consumers: the shader's grass fetches, the blade species
// scores and the clutter rules — ground and growth tell the same story.
struct GrassZone {
    u32 variantA { 0 };
    u32 variantB { 0 };
    f32 blendA { 1.0f }; // 1 = pure A; < 1 inside the border band only
};
GrassZone grassZoneAt(f32 x, f32 z);

// The weights as the SHADER shows them: the raw
// altitude borders perturbed by the splat wander term (terrain.frag) so
// a footstep on VISIBLE snow sounds like snow, not like the contour
// line. `splatUvScale` = uTerrainInfo.z (tiles/meter, the render knob).
// Scatter rules keep using materialWeights above (their masks predate
// the wander and reseeding them would move every bush).
// Deliberately does NOT follow the shader's height-based layer blend
// (terrain_blend.glsl): that redistribution is decimeter-scale inside the
// transition bands — the audio verdict there is perceptually ambiguous
// and mirroring it would mean sampling the displacement tiles CPU-side
// for no audible gain. The dominant layer outside the bands is unchanged.
MaterialWeights materialWeightsShaded(const TerrainParams& params, f32 x,
                                      f32 z, f32 splatUvScale);

} // namespace terrain

} // namespace render
