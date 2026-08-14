#pragma once

#include <cmath>

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"
#include "engine/core/Hash.hpp"

// Deterministic value-noise primitives, extracted from TerrainNoise.cpp to
// this headless home so the terrain generator (engine/terrain/generation)
// can share the exact hash family without depending on engine/render/.
// TerrainNoise.cpp composes THESE functions — any change here moves the
// whole existing landscape (the pinned village-level doctest guards it).

namespace render::noise {

// Hash of a lattice point, in [0, 1].
inline f32 lattice(u32 seed, i32 xi, i32 zi) {
    u32 h = seed;
    h = core::hashU32(h ^ static_cast<u32>(xi));
    h = core::hashU32(h ^ static_cast<u32>(zi));
    return static_cast<f32>(h) * (1.0f / 4294967295.0f);
}

// Smooth bilinear value noise in [0, 1]; quintic fade (C2-continuous, so
// analytic normals from central differences stay smooth).
inline f32 value(u32 seed, f32 x, f32 z) {
    const f32 fx = std::floor(x);
    const f32 fz = std::floor(z);
    const i32 xi = static_cast<i32>(fx);
    const i32 zi = static_cast<i32>(fz);
    const f32 tx = x - fx;
    const f32 tz = z - fz;
    const f32 ux = tx * tx * tx * (tx * (tx * 6.0f - 15.0f) + 10.0f);
    const f32 uz = tz * tz * tz * (tz * (tz * 6.0f - 15.0f) + 10.0f);

    const f32 v00 = lattice(seed, xi, zi);
    const f32 v10 = lattice(seed, xi + 1, zi);
    const f32 v01 = lattice(seed, xi, zi + 1);
    const f32 v11 = lattice(seed, xi + 1, zi + 1);
    const f32 a = v00 + (v10 - v00) * ux;
    const f32 b = v01 + (v11 - v01) * ux;
    return a + (b - a) * uz;
}

// FBM in [0, 1] (normalized by total amplitude).
inline f32 fbm(u32 seed, f32 x, f32 z, f32 frequency, i32 octaves,
               f32 lacunarity, f32 gain) {
    f32 sum = 0.0f;
    f32 amplitude = 1.0f;
    f32 total = 0.0f;
    for (i32 i = 0; i < octaves; ++i) {
        sum += value(seed + static_cast<u32>(i) * 0x9e3779b9u,
                     x * frequency, z * frequency) *
               amplitude;
        total += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return sum / total;
}

// Ridged FBM in [0, 1]: sharp crests where the noise crosses its midline.
inline f32 ridgedFbm(u32 seed, f32 x, f32 z, f32 frequency, i32 octaves,
                     f32 lacunarity, f32 gain) {
    f32 sum = 0.0f;
    f32 amplitude = 1.0f;
    f32 total = 0.0f;
    for (i32 i = 0; i < octaves; ++i) {
        const f32 n = value(seed + static_cast<u32>(i) * 0x85ebca6bu,
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

// 3D members of the same hash family (dungeon carving needs volumetric
// noise). New functions only: nothing above composes them, so the existing
// landscape cannot move.
inline f32 lattice3(u32 seed, i32 xi, i32 yi, i32 zi) {
    u32 h = seed;
    h = core::hashU32(h ^ static_cast<u32>(xi));
    h = core::hashU32(h ^ static_cast<u32>(yi));
    h = core::hashU32(h ^ static_cast<u32>(zi));
    return static_cast<f32>(h) * (1.0f / 4294967295.0f);
}

inline f32 value3(u32 seed, f32 x, f32 y, f32 z) {
    const f32 fx = std::floor(x);
    const f32 fy = std::floor(y);
    const f32 fz = std::floor(z);
    const i32 xi = static_cast<i32>(fx);
    const i32 yi = static_cast<i32>(fy);
    const i32 zi = static_cast<i32>(fz);
    const f32 tx = x - fx;
    const f32 ty = y - fy;
    const f32 tz = z - fz;
    const f32 ux = tx * tx * tx * (tx * (tx * 6.0f - 15.0f) + 10.0f);
    const f32 uy = ty * ty * ty * (ty * (ty * 6.0f - 15.0f) + 10.0f);
    const f32 uz = tz * tz * tz * (tz * (tz * 6.0f - 15.0f) + 10.0f);

    const auto plane = [&](i32 yo) {
        const f32 v00 = lattice3(seed, xi, yi + yo, zi);
        const f32 v10 = lattice3(seed, xi + 1, yi + yo, zi);
        const f32 v01 = lattice3(seed, xi, yi + yo, zi + 1);
        const f32 v11 = lattice3(seed, xi + 1, yi + yo, zi + 1);
        const f32 a = v00 + (v10 - v00) * ux;
        const f32 b = v01 + (v11 - v01) * ux;
        return a + (b - a) * uz;
    };
    const f32 lo = plane(0);
    const f32 hi = plane(1);
    return lo + (hi - lo) * uy;
}

inline f32 fbm3(u32 seed, const glm::vec3& p, f32 frequency, i32 octaves,
                f32 lacunarity, f32 gain) {
    f32 sum = 0.0f;
    f32 amplitude = 1.0f;
    f32 total = 0.0f;
    for (i32 i = 0; i < octaves; ++i) {
        sum += value3(seed + static_cast<u32>(i) * 0x9e3779b9u,
                      p.x * frequency, p.y * frequency, p.z * frequency) *
               amplitude;
        total += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return sum / total;
}

inline f32 smoothstep01(f32 low, f32 high, f32 x) {
    const f32 t = glm::clamp((x - low) / (high - low), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace render::noise
