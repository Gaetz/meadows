#include "engine/render/landscape/TerrainNoise.hpp"

#include "engine/render/landscape/SplatTextures.hpp" // splatWander

#include <cmath>

#include "engine/core/Hash.hpp"

namespace render {

namespace {

// hashU32 lives in engine/core/Hash.hpp (shared scatter hash family).
using core::hashU32;

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

f32 noise01(u32 seed, f32 x, f32 z) {
    return valueNoise(seed, x, z);
}

MaterialWeights materialWeights(const TerrainParams& params, f32 height,
                                const Vec3& normal) {
    const f32 slope = 1.0f - normal.y;
    MaterialWeights weights;
    weights.rock = glm::smoothstep(0.18f, 0.35f, slope);
    weights.snow = glm::smoothstep(kSnowLine - 12.0f, kSnowLine + 42.0f,
                                   height) *
                   (1.0f - glm::smoothstep(0.25f, 0.45f, slope));
    weights.sand = (1.0f - glm::smoothstep(params.seaLevel + 1.0f,
                                           params.seaLevel + 8.0f, height)) *
                   (1.0f - weights.rock);
    weights.grass = glm::max(
        1.0f - weights.rock - weights.snow - weights.sand, 0.0f);
    return weights;
}

MaterialWeights materialWeightsShaded(const TerrainParams& params, f32 x,
                                      f32 z, f32 splatUvScale) {
    // Keep in LOCKSTEP with terrain.frag: uv = xz * scale; wander =
    // splat green at uv * 0.06 - 0.5; the snow/sand altitudes shift by
    // wander * 26 / * 5 before the same smoothsteps.
    const f32 h = height(params, x, z);
    const Vec3 n = normal(params, x, z);
    const f32 slope = 1.0f - n.y;
    const f32 u = x * splatUvScale * 0.06f;
    const f32 v = z * splatUvScale * 0.06f;
    const f32 wander =
        splatWander(u - std::floor(u), v - std::floor(v)) - 0.5f;
    MaterialWeights weights;
    weights.rock = glm::smoothstep(0.18f, 0.35f, slope);
    weights.snow = glm::smoothstep(kSnowLine - 12.0f, kSnowLine + 42.0f,
                                   h + wander * 26.0f) *
                   (1.0f - glm::smoothstep(0.25f, 0.45f, slope));
    weights.sand =
        (1.0f - glm::smoothstep(params.seaLevel + 1.0f,
                                params.seaLevel + 8.0f,
                                h + wander * 5.0f)) *
        (1.0f - weights.rock);
    weights.grass = glm::max(
        1.0f - weights.rock - weights.snow - weights.sand, 0.0f);
    return weights;
}

// Bilinear sample of the authored delta grid covering (x, z); 0 where no
// chunk is authored. Edge samples are shared between neighbours, so the
// blend is seamless across chunk borders.
f32 authoredDelta(const HeightPatches& patches, f32 x, f32 z) {
    const f32 size = patches.chunkSize;
    const i32 cx = static_cast<i32>(std::floor(x / size));
    const i32 cz = static_cast<i32>(std::floor(z / size));
    const auto it = patches.chunks.find(HeightPatches::keyOf(cx, cz));
    if (it == patches.chunks.end() || it->second.samples < 2) {
        return 0.0f;
    }
    const HeightPatch& patch = it->second;
    const f32 n1 = static_cast<f32>(patch.samples - 1);
    const f32 u = (x - static_cast<f32>(cx) * size) / size * n1;
    const f32 v = (z - static_cast<f32>(cz) * size) / size * n1;
    const u32 u0 = glm::min(static_cast<u32>(u), patch.samples - 2);
    const u32 v0 = glm::min(static_cast<u32>(v), patch.samples - 2);
    const f32 tu = u - static_cast<f32>(u0);
    const f32 tv = v - static_cast<f32>(v0);
    const auto at = [&](u32 col, u32 row) {
        return patch.deltas[row * patch.samples + col];
    };
    const f32 a = at(u0, v0) + (at(u0 + 1, v0) - at(u0, v0)) * tu;
    const f32 b = at(u0, v0 + 1) + (at(u0 + 1, v0 + 1) - at(u0, v0 + 1)) * tu;
    return a + (b - a) * tv;
}

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

    const f32 base = hills + mask * mountains;
    return params.patches ? base + authoredDelta(*params.patches, x, z)
                          : base;
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
