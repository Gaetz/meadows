#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"
#include "engine/terrain/HeightPatches.hpp" // render::HeightPatch / HeightPatches

namespace render {

// Authored terrain overrides (chantier 2 B8): per-chunk DELTA grids added
// on top of the procedural base — final height = noise + bilinear(delta).
// The world layer builds this from TerrainPatchForm records + `.ter`
// assets (the engine never sees Forms — rule n°2, HORIZONTAL-PASS);
// sculpting edits grids then publishes a NEW immutable instance, so
// workers holding the old pointer stay race-free.
// HeightPatch / HeightPatches now live in engine/terrain/HeightPatches.hpp
// (a headless home) so the world layer can build them without depending on
// engine/render/. They still ride inside TerrainParams below.

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
    // worker-held ones included, now keeps the data alive by itself.
    // Null = pure procedural, bit-identical to the pre-B8 behavior (the
    // non-regression contract). Riding inside TerrainParams means every
    // consumer (workers included) is patched without a signature change.
    sptr<const HeightPatches> patches;

    // Rolling hills: FBM value noise.
    f32 hillWavelength { 400.0f }; // meters per base octave
    f32 hillAmplitude { 50.0f };
    i32 octaves { 5 };
    f32 lacunarity { 2.0f };
    f32 gain { 0.5f };

    // Ridged mountains, gated by a low-frequency mask so ranges rise in
    // some regions and leave plains elsewhere.
    f32 mountainWavelength { 1500.0f };
    f32 mountainAmplitude { 180.0f };
    f32 mountainMaskLow { 0.45f };  // mask noise below this -> plains
    f32 mountainMaskHigh { 0.75f }; // above this -> full mountains

    f32 seaLevel { 14.0f };
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

// Raw smooth value noise in [0,1] — the building block, exposed for scatter
// masks (grass patches, forest belts) so they share the terrain's hash.
f32 noise01(u32 seed, f32 x, f32 z);

// CPU mirror of terrain.frag's splat weights (minus the texture-driven
// border wander): ONE definition of "what grows where" shared by every
// scatter rule. Keep in sync with the shader when tuning.
struct MaterialWeights {
    f32 grass { 0.0f };
    f32 rock { 0.0f };
    f32 snow { 0.0f };
    f32 sand { 0.0f };
};
constexpr f32 kSnowLine = 110.0f; // meters; matches uTerrainInfo.y
MaterialWeights materialWeights(const TerrainParams& params, f32 height,
                                const Vec3& normal);

// The weights as the SHADER shows them (P0 C4b follow-up): the raw
// altitude borders perturbed by the splat wander term (terrain.frag) so
// a footstep on VISIBLE snow sounds like snow, not like the contour
// line. `splatUvScale` = uTerrainInfo.z (tiles/meter, the render knob).
// Scatter rules keep using materialWeights above (their masks predate
// the wander and reseeding them would move every bush).
MaterialWeights materialWeightsShaded(const TerrainParams& params, f32 x,
                                      f32 z, f32 splatUvScale);

} // namespace terrain

} // namespace render
