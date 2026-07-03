#include "engine/render/landscape/SplatTextures.hpp"

#include <cmath>

#include <glm/glm.hpp>

namespace render {

namespace {

// Same integer hash family as TerrainNoise, kept local: tile synthesis is
// its own little world (periodic lattice, texel space).
u32 hashU32(u32 v) {
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}

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

void writeTexel(vector<u8>& pixels, size_t index, const Vec3& color) {
    pixels[index + 0] = static_cast<u8>(glm::clamp(color.r, 0.0f, 1.0f) * 255.0f);
    pixels[index + 1] = static_cast<u8>(glm::clamp(color.g, 0.0f, 1.0f) * 255.0f);
    pixels[index + 2] = static_cast<u8>(glm::clamp(color.b, 0.0f, 1.0f) * 255.0f);
    pixels[index + 3] = 255;
}

Vec3 grassTexel(f32 u, f32 v) {
    // Soft mottled green with drier yellowish patches.
    const f32 blotch = tileFbm(101, u, v, 6, 4);
    const f32 grain = tileFbm(102, u, v, 32, 2);
    Vec3 color = glm::mix(Vec3 { 0.30f, 0.47f, 0.19f },
                          Vec3 { 0.42f, 0.48f, 0.20f },
                          glm::smoothstep(0.45f, 0.70f, blotch));
    return color * (0.92f + 0.16f * grain);
}

Vec3 rockTexel(f32 u, f32 v) {
    // Gray base, ridged strata, darker crack lines.
    const f32 strata = tileFbm(201, u, v * 2.5f, 8, 4);
    const f32 ridge = 1.0f - std::abs(2.0f * strata - 1.0f);
    const f32 crack = tileFbm(202, u, v, 16, 3);
    Vec3 color = Vec3 { 0.46f, 0.44f, 0.42f } * (0.82f + 0.30f * ridge);
    if (crack < 0.32f) {
        color *= 0.72f + 0.28f * (crack / 0.32f);
    }
    return color;
}

Vec3 snowTexel(f32 u, f32 v) {
    // Near-white with the faintest blue undulation and sparse sparkle.
    const f32 drift = tileFbm(301, u, v, 5, 3);
    const f32 sparkle = latticeValue(302, static_cast<i32>(u * 256.0f),
                                     static_cast<i32>(v * 256.0f));
    Vec3 color = glm::mix(Vec3 { 0.88f, 0.91f, 0.96f },
                          Vec3 { 0.96f, 0.97f, 1.00f }, drift);
    if (sparkle > 0.993f) {
        color = Vec3 { 1.0f, 1.0f, 1.0f };
    }
    return color;
}

Vec3 sandTexel(f32 u, f32 v) {
    // Warm beige, fine grain, gentle wind-ripple banding.
    const f32 grain = tileFbm(401, u, v, 48, 2);
    const f32 wobble = tileFbm(402, u, v, 4, 2);
    const f32 ripple =
        0.5f + 0.5f * std::sin((v + wobble * 0.35f) * 12.0f * 6.2831853f);
    Vec3 color = Vec3 { 0.78f, 0.70f, 0.52f } * (0.94f + 0.10f * grain);
    return color * (0.96f + 0.06f * ripple);
}

} // namespace

vector<u8> buildSplatTilePixels() {
    constexpr u32 kSize = kSplatTileSize;
    vector<u8> pixels(static_cast<size_t>(kSize) * kSize * 4 *
                      SplatLayer_Count);
    const size_t layerBytes = static_cast<size_t>(kSize) * kSize * 4;
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const f32 u = static_cast<f32>(x) / kSize;
            const f32 v = static_cast<f32>(y) / kSize;
            const size_t texel = (static_cast<size_t>(y) * kSize + x) * 4;
            writeTexel(pixels, SplatLayer_Grass * layerBytes + texel,
                       grassTexel(u, v));
            writeTexel(pixels, SplatLayer_Rock * layerBytes + texel,
                       rockTexel(u, v));
            writeTexel(pixels, SplatLayer_Snow * layerBytes + texel,
                       snowTexel(u, v));
            writeTexel(pixels, SplatLayer_Sand * layerBytes + texel,
                       sandTexel(u, v));
        }
    }
    return pixels;
}

} // namespace render
