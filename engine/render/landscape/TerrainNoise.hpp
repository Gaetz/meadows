#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

namespace render {

// Terrain generation parameters. One plain struct on purpose: when landscape
// population becomes moddable it converts into a reflected Form patched
// through the §5 plugin system (precedent: StatsTuningForm), and this stays a
// local change.
struct TerrainParams {
    u32 seed { 1337 };

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

    f32 seaLevel { 8.0f };
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

} // namespace terrain

} // namespace render
