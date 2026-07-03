#include "engine/render/landscape/TerrainNoise.hpp"

#include <cmath>

namespace render {

namespace {

// Integer hash (Wang-style avalanche); the only randomness source here.
u32 hashU32(u32 v) {
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}

// Hash of a lattice point, in [0, 1].
f32 latticeValue(u32 seed, i32 xi, i32 zi) {
    u32 h = seed;
    h = hashU32(h ^ static_cast<u32>(xi));
    h = hashU32(h ^ static_cast<u32>(zi));
    return static_cast<f32>(h) * (1.0f / 4294967295.0f);
}

// Smooth bilinear value noise in [0, 1]; quintic fade (C2-continuous, so
// analytic normals from central differences stay smooth).
f32 valueNoise(u32 seed, f32 x, f32 z) {
    const f32 fx = std::floor(x);
    const f32 fz = std::floor(z);
    const i32 xi = static_cast<i32>(fx);
    const i32 zi = static_cast<i32>(fz);
    const f32 tx = x - fx;
    const f32 tz = z - fz;
    const f32 ux = tx * tx * tx * (tx * (tx * 6.0f - 15.0f) + 10.0f);
    const f32 uz = tz * tz * tz * (tz * (tz * 6.0f - 15.0f) + 10.0f);

    const f32 v00 = latticeValue(seed, xi, zi);
    const f32 v10 = latticeValue(seed, xi + 1, zi);
    const f32 v01 = latticeValue(seed, xi, zi + 1);
    const f32 v11 = latticeValue(seed, xi + 1, zi + 1);
    const f32 a = v00 + (v10 - v00) * ux;
    const f32 b = v01 + (v11 - v01) * ux;
    return a + (b - a) * uz;
}

// FBM in [0, 1] (normalized by total amplitude).
f32 fbm(u32 seed, f32 x, f32 z, f32 frequency, i32 octaves, f32 lacunarity,
        f32 gain) {
    f32 sum = 0.0f;
    f32 amplitude = 1.0f;
    f32 total = 0.0f;
    for (i32 i = 0; i < octaves; ++i) {
        sum += valueNoise(seed + static_cast<u32>(i) * 0x9e3779b9u,
                          x * frequency, z * frequency) *
               amplitude;
        total += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return sum / total;
}

// Ridged FBM in [0, 1]: sharp crests where the noise crosses its midline.
f32 ridgedFbm(u32 seed, f32 x, f32 z, f32 frequency, i32 octaves,
              f32 lacunarity, f32 gain) {
    f32 sum = 0.0f;
    f32 amplitude = 1.0f;
    f32 total = 0.0f;
    for (i32 i = 0; i < octaves; ++i) {
        const f32 n = valueNoise(seed + static_cast<u32>(i) * 0x85ebca6bu,
                                 x * frequency, z * frequency);
        f32 ridge = 1.0f - std::abs(2.0f * n - 1.0f);
        ridge *= ridge; // sharpen crests
        sum += ridge * amplitude;
        total += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return sum / total;
}

f32 smoothstepf(f32 low, f32 high, f32 x) {
    const f32 t = glm::clamp((x - low) / (high - low), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

namespace terrain {

f32 height(const TerrainParams& params, f32 x, f32 z) {
    const f32 hills = (fbm(params.seed, x, z, 1.0f / params.hillWavelength,
                           params.octaves, params.lacunarity, params.gain) *
                           2.0f -
                       1.0f) *
                      params.hillAmplitude;

    const f32 mask = smoothstepf(
        params.mountainMaskLow, params.mountainMaskHigh,
        fbm(params.seed ^ 0x51ed270bu, x, z, 1.0f / params.mountainWavelength,
            3, params.lacunarity, params.gain));
    const f32 mountains =
        ridgedFbm(params.seed ^ 0xc2b2ae35u, x, z,
                  2.0f / params.mountainWavelength, 4, params.lacunarity,
                  params.gain) *
        params.mountainAmplitude;

    return hills + mask * mountains;
}

Vec3 normal(const TerrainParams& params, f32 x, f32 z, f32 step) {
    const f32 hl = height(params, x - step, z);
    const f32 hr = height(params, x + step, z);
    const f32 hd = height(params, x, z - step);
    const f32 hu = height(params, x, z + step);
    return glm::normalize(Vec3 { hl - hr, 2.0f * step, hd - hu });
}

} // namespace terrain

} // namespace render
