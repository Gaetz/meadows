#include "engine/render/landscape/SplatTextures.hpp"

#include <cmath>

#include <glm/glm.hpp>

#include "engine/core/Hash.hpp"

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

void writeTexel(vector<u8>& pixels, size_t index, const Vec3& color) {
    pixels[index + 0] = static_cast<u8>(glm::clamp(color.r, 0.0f, 1.0f) * 255.0f);
    pixels[index + 1] = static_cast<u8>(glm::clamp(color.g, 0.0f, 1.0f) * 255.0f);
    pixels[index + 2] = static_cast<u8>(glm::clamp(color.b, 0.0f, 1.0f) * 255.0f);
    pixels[index + 3] = 255;
}

Vec3 grassTexel(f32 u, f32 v) {
    return grassAlbedo(u, v);
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

Vec3 cliffTexel(f32 u, f32 v) {
    // Stratified cliff stone: pronounced horizontal banding + vertical
    // fracture streaks — paler and warmer than the scree rock so bare
    // faces read as geology, not just steeper rock.
    const f32 bands = tileFbm(501, u * 0.5f, v * 3.5f, 8, 4);
    const f32 ledge = 1.0f - std::abs(2.0f * bands - 1.0f);
    const f32 fracture = tileFbm(502, u * 3.0f, v * 0.8f, 12, 3);
    Vec3 color = Vec3 { 0.55f, 0.50f, 0.44f } * (0.78f + 0.28f * ledge);
    if (fracture < 0.28f) {
        color *= 0.70f + 0.30f * (fracture / 0.28f);
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
            writeTexel(pixels, SplatLayer_Cliff * layerBytes + texel,
                       cliffTexel(u, v));
        }
    }
    return pixels;
}


Vec3 grassAlbedo(f32 u, f32 v) {
    // Near-uniform forest green (#6FA160 — the midpoint between the old
    // meadow green and the tree-foliage palette, so meadow and canopies
    // read as one family): one hue, a WHISPER of blotch luminance drift
    // (~±1%, mean 1) — just enough to break the perfectly flat albedo
    // that exposed the RC probe-parity speckle, invisible as texture.
    // The GREEN channel feeds the border wander — its MEAN is unchanged
    // by the symmetric drift, so the -0.67 centering in terrain.frag /
    // TerrainNoise.cpp stays valid.
    const f32 blotch = tileFbm(101, u, v, 6, 4);
    return Vec3 { 0.434f, 0.633f, 0.375f } *
           (0.99f + 0.02f * glm::smoothstep(0.30f, 0.70f, blotch));
}

f32 splatWander(f32 u, f32 v) {
    // The texture stores DISPLAY-space bytes in an SRGBA8 view; the
    // shader's sampler hands back linear — decode the same way.
    const f32 display = glm::clamp(grassTexel(u, v).y, 0.0f, 1.0f);
    return display <= 0.04045f
               ? display / 12.92f
               : std::pow((display + 0.055f) / 1.055f, 2.4f);
}

} // namespace render
