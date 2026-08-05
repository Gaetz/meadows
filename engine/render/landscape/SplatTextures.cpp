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

f32 grassHeight(f32 u, f32 v); // defined below (variants reuse it)

// Grass-family variant tiles (zoned by terrain_zones.glsl): the SAME
// green family — variation lives in CONTENT and RELIEF, never in color
// patches (dev directive). v1 = worn grass showing soil streaks (leaves
// are reserved for a future undergrowth set), v2 = grass with surfacing
// stones, v3 = grass thinning into dirt.
Vec3 wornGrassTexel(f32 u, f32 v) {
    const f32 wear = tileFbm(701, u * 0.7f, v * 1.6f, 12, 3);
    const f32 wornMask = glm::smoothstep(0.52f, 0.70f, wear);
    const f32 grain = tileFbm(703, u, v, 48, 2);
    const Vec3 soil = Vec3 { 0.45f, 0.38f, 0.26f } *
                      (0.92f + 0.14f * grain);
    return glm::mix(grassAlbedo(u, v), soil, wornMask * 0.7f);
}

Vec3 stonyGrassTexel(f32 u, f32 v) {
    const f32 stones = tileFbm(901, u, v, 20, 3);
    const f32 stoneMask = glm::smoothstep(0.60f, 0.74f, stones);
    const f32 grain = tileFbm(902, u, v, 48, 2);
    const Vec3 stone =
        Vec3 { 0.52f, 0.50f, 0.47f } * (0.9f + 0.2f * grain);
    return glm::mix(grassAlbedo(u, v), stone, stoneMask * 0.85f);
}

Vec3 dirtGrassTexel(f32 u, f32 v) {
    // Grass thinning into bare earth patches.
    const f32 patches = tileFbm(801, u, v, 10, 3);
    const f32 grain = tileFbm(802, u, v, 48, 2);
    const Vec3 dirt = Vec3 { 0.42f, 0.34f, 0.24f } *
                      (0.92f + 0.14f * grain);
    return glm::mix(grassAlbedo(u, v), dirt,
                    glm::smoothstep(0.45f, 0.7f, patches) * 0.7f);
}

f32 wornGrassHeight(f32 u, f32 v) {
    const f32 wear = tileFbm(701, u * 0.7f, v * 1.6f, 12, 3);
    // Worn streaks sit LOWER than the grass around them.
    return glm::mix(grassHeight(u, v), 0.22f,
                    glm::smoothstep(0.52f, 0.70f, wear) * 0.7f);
}

f32 stonyGrassHeight(f32 u, f32 v) {
    const f32 stones = tileFbm(901, u, v, 20, 3);
    return grassHeight(u, v) * 0.6f +
           0.45f * glm::smoothstep(0.60f, 0.74f, stones);
}

f32 dirtGrassHeight(f32 u, f32 v) {
    const f32 patches = tileFbm(801, u, v, 10, 3);
    const f32 grain = tileFbm(802, u, v, 48, 2);
    return glm::mix(grassHeight(u, v) * 0.8f, 0.25f + 0.08f * grain,
                    glm::smoothstep(0.45f, 0.7f, patches) * 0.7f);
}

// Per-layer displacement, correlated with the albedo functions above by
// reusing their noise seeds — the blend then reveals the SAME structures
// the color shows (ridges poke through, cracks recede).
f32 grassHeight(f32 u, f32 v) {
    const f32 blotch = tileFbm(101, u, v, 6, 4);
    const f32 clump = tileFbm(103, u, v, 24, 3);
    return 0.35f + 0.30f * blotch + 0.20f * clump;
}

f32 rockHeight(f32 u, f32 v) {
    const f32 strata = tileFbm(201, u, v * 2.5f, 8, 4);
    const f32 ridge = 1.0f - std::abs(2.0f * strata - 1.0f);
    const f32 crack = tileFbm(202, u, v, 16, 3);
    f32 h = 0.30f + 0.55f * ridge;
    if (crack < 0.32f) {
        h -= 0.25f * (1.0f - crack / 0.32f);
    }
    return h;
}

f32 snowHeight(f32 u, f32 v) {
    const f32 drift = tileFbm(301, u, v, 5, 3);
    return 0.55f + 0.30f * drift; // snow sits high: it covers
}

f32 sandHeight(f32 u, f32 v) {
    const f32 grain = tileFbm(401, u, v, 48, 2);
    const f32 wobble = tileFbm(402, u, v, 4, 2);
    const f32 ripple =
        0.5f + 0.5f * std::sin((v + wobble * 0.35f) * 12.0f * 6.2831853f);
    return 0.25f + 0.18f * ripple + 0.07f * grain;
}

f32 cliffHeight(f32 u, f32 v) {
    const f32 bands = tileFbm(501, u * 0.5f, v * 3.5f, 8, 4);
    const f32 ledge = 1.0f - std::abs(2.0f * bands - 1.0f);
    const f32 fracture = tileFbm(502, u * 3.0f, v * 0.8f, 12, 3);
    f32 h = 0.35f + 0.50f * ledge;
    if (fracture < 0.28f) {
        h -= 0.22f * (1.0f - fracture / 0.28f);
    }
    return h;
}

} // namespace

vector<f32> buildSplatHeightPixels() {
    constexpr u32 kSize = kSplatTileSize;
    vector<f32> pixels(static_cast<size_t>(kSize) * kSize *
                       kSplatArrayLayers);
    const size_t layerTexels = static_cast<size_t>(kSize) * kSize;
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const f32 u = static_cast<f32>(x) / kSize;
            const f32 v = static_cast<f32>(y) / kSize;
            const size_t texel = static_cast<size_t>(y) * kSize + x;
            pixels[SplatLayer_Grass * layerTexels + texel] =
                glm::clamp(grassHeight(u, v), 0.0f, 1.0f);
            pixels[SplatLayer_Rock * layerTexels + texel] =
                glm::clamp(rockHeight(u, v), 0.0f, 1.0f);
            pixels[SplatLayer_Snow * layerTexels + texel] =
                glm::clamp(snowHeight(u, v), 0.0f, 1.0f);
            pixels[SplatLayer_Sand * layerTexels + texel] =
                glm::clamp(sandHeight(u, v), 0.0f, 1.0f);
            pixels[SplatLayer_Cliff * layerTexels + texel] =
                glm::clamp(cliffHeight(u, v), 0.0f, 1.0f);
            pixels[grassVariantLayer(1) * layerTexels + texel] =
                glm::clamp(wornGrassHeight(u, v), 0.0f, 1.0f);
            pixels[grassVariantLayer(2) * layerTexels + texel] =
                glm::clamp(stonyGrassHeight(u, v), 0.0f, 1.0f);
            pixels[grassVariantLayer(3) * layerTexels + texel] =
                glm::clamp(dirtGrassHeight(u, v), 0.0f, 1.0f);
            // Rock/snow/sand family variants: the procedural set reuses
            // the base tiles (the hex uv offsets still de-tile them);
            // real content variety lives in the cooked library.
            pixels[familyVariantLayer(1) * layerTexels + texel] =
                glm::clamp(rockHeight(u, v), 0.0f, 1.0f);
            pixels[familyVariantLayer(2) * layerTexels + texel] =
                glm::clamp(snowHeight(u, v), 0.0f, 1.0f);
            pixels[familyVariantLayer(3) * layerTexels + texel] =
                glm::clamp(sandHeight(u, v), 0.0f, 1.0f);
        }
    }
    return pixels;
}

vector<u8> buildSplatNormalPixels() {
    constexpr u32 kSize = kSplatTileSize;
    // Height-to-slope gain: the [0,1] heights read as decimeter-scale
    // relief on a 4 m tile.
    constexpr f32 kStrength = 2.0f; // hand-tuned
    constexpr f32 kStep = 1.0f / kSize;
    vector<u8> pixels(static_cast<size_t>(kSize) * kSize * 4 *
                      kSplatArrayLayers);
    const size_t layerBytes = static_cast<size_t>(kSize) * kSize * 4;
    const auto wrap = [](f32 t) { return t - std::floor(t); };
    using HeightFn = f32 (*)(f32, f32);
    constexpr array<HeightFn, kSplatArrayLayers> kHeightFn {
        grassHeight,      rockHeight,      snowHeight,
        sandHeight,       cliffHeight,     wornGrassHeight,
        stonyGrassHeight, dirtGrassHeight, rockHeight,
        snowHeight,       sandHeight
    };
    for (u32 layer = 0; layer < kSplatArrayLayers; ++layer) {
        const HeightFn fn = kHeightFn[layer];
        for (u32 y = 0; y < kSize; ++y) {
            for (u32 x = 0; x < kSize; ++x) {
                const f32 u = static_cast<f32>(x) / kSize;
                const f32 v = static_cast<f32>(y) / kSize;
                const f32 dx = (fn(wrap(u + kStep), v) -
                                fn(wrap(u - kStep + 1.0f), v)) *
                               kStrength;
                const f32 dy = (fn(u, wrap(v + kStep)) -
                                fn(u, wrap(v - kStep + 1.0f))) *
                               kStrength;
                const Vec3 n =
                    glm::normalize(Vec3 { -dx, -dy, 2.0f * kStep * 32.0f });
                const size_t at = layer * layerBytes +
                                  (static_cast<size_t>(y) * kSize + x) * 4;
                pixels[at + 0] = static_cast<u8>(
                    glm::clamp(n.x * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
                pixels[at + 1] = static_cast<u8>(
                    glm::clamp(n.y * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
                pixels[at + 2] = 255; // z reconstructed in-shader
                pixels[at + 3] = 255;
            }
        }
    }
    return pixels;
}

vector<u8> buildSplatTilePixels() {
    constexpr u32 kSize = kSplatTileSize;
    vector<u8> pixels(static_cast<size_t>(kSize) * kSize * 4 *
                      kSplatArrayLayers);
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
            writeTexel(pixels, grassVariantLayer(1) * layerBytes + texel,
                       wornGrassTexel(u, v));
            writeTexel(pixels, grassVariantLayer(2) * layerBytes + texel,
                       stonyGrassTexel(u, v));
            writeTexel(pixels, grassVariantLayer(3) * layerBytes + texel,
                       dirtGrassTexel(u, v));
            writeTexel(pixels, familyVariantLayer(1) * layerBytes + texel,
                       rockTexel(u, v));
            writeTexel(pixels, familyVariantLayer(2) * layerBytes + texel,
                       snowTexel(u, v));
            writeTexel(pixels, familyVariantLayer(3) * layerBytes + texel,
                       sandTexel(u, v));
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
    return Vec3 { 0.434f, 0.633f, 0.375f } * grassBlotch(u, v);
}

f32 grassBlotch(f32 u, f32 v) {
    const f32 blotch = tileFbm(101, u, v, 6, 4);
    return 0.99f + 0.02f * glm::smoothstep(0.30f, 0.70f, blotch);
}

} // namespace render
