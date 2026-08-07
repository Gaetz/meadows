#include "engine/render/landscape/SplatTextures.hpp"

#include <cmath>

#include <glm/glm.hpp>

#include "engine/core/Hash.hpp"

// The procedural splat tile synthesis is GONE (dev decision 2026-08-06):
// the cooked .mtex library is the one material set — TerrainSystem falls
// back to flat placeholder arrays when it is absent. Only the grass root
// color source survives here: the blades bake it at scatter, on the
// cooked set too.

namespace render {

namespace {

// hashU32 lives in engine/core/Hash.hpp (shared scatter hash family).
using core::hashU32;

f32 latticeValue(u32 seed, i32 x, i32 y) {
    u32 h = seed;
    h = hashU32(h ^ static_cast<u32>(x));
    h = hashU32(h ^ static_cast<u32>(y));
    return static_cast<f32>(h) * (1.0f / 4294967295.0f);
}

// Value noise whose lattice wraps at `period` — the output tiles seamlessly.
f32 tileNoise(u32 seed, f32 x, f32 y, i32 period) {
    const f32 fx = std::floor(x);
    const f32 fy = std::floor(y);
    const i32 x0 = static_cast<i32>(fx) % period;
    const i32 y0 = static_cast<i32>(fy) % period;
    const i32 x1 = (x0 + 1) % period;
    const i32 y1 = (y0 + 1) % period;
    const f32 tx = x - fx;
    const f32 ty = y - fy;
    const f32 ux = tx * tx * tx * (tx * (tx * 6.0f - 15.0f) + 10.0f);
    const f32 uy = ty * ty * ty * (ty * (ty * 6.0f - 15.0f) + 10.0f);

    const f32 v00 = latticeValue(seed, x0, y0);
    const f32 v10 = latticeValue(seed, x1, y0);
    const f32 v01 = latticeValue(seed, x0, y1);
    const f32 v11 = latticeValue(seed, x1, y1);
    const f32 a = v00 + (v10 - v00) * ux;
    const f32 b = v01 + (v11 - v01) * ux;
    return a + (b - a) * uy;
}

// Tileable FBM in [0,1]; period doubles per octave so every octave wraps.
f32 tileFbm(u32 seed, f32 u, f32 v, i32 basePeriod, i32 octaves) {
    f32 sum = 0.0f;
    f32 amplitude = 1.0f;
    f32 total = 0.0f;
    f32 frequency = static_cast<f32>(basePeriod);
    i32 period = basePeriod;
    for (i32 i = 0; i < octaves; ++i) {
        sum += tileNoise(seed + static_cast<u32>(i) * 0x9e3779b9u,
                         u * frequency, v * frequency, period) *
               amplitude;
        total += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
        period *= 2;
    }
    return sum / total;
}

} // namespace

Vec3 grassAlbedo(f32 u, f32 v) {
    // Near-uniform forest green (#6FA160 — the midpoint between the old
    // meadow green and the tree-foliage palette, so meadow and canopies
    // read as one family): one hue, a WHISPER of blotch luminance drift
    // (~±1%, mean 1) — just enough to break the perfectly flat albedo
    // that exposed the RC probe-parity speckle, invisible as texture.
    return Vec3 { 0.434f, 0.633f, 0.375f } * grassBlotch(u, v);
}

f32 grassBlotch(f32 u, f32 v) {
    const f32 blotch = tileFbm(101, u, v, 6, 4);
    return 0.99f + 0.02f * glm::smoothstep(0.30f, 0.70f, blotch);
}

} // namespace render
