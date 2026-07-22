#include "game/MapRaster.hpp"

#include <glm/glm.hpp>

namespace game {

namespace {

// [cpp-tuning] Map palette + shading. The stylized-parchment-free look:
// muted blues for water, the game's splat families for land, a soft
// sun-from-north-west shade and a quantized hypsometric lift so altitude
// reads in bands. Repaint freely at validation — everything visual about
// the map lives here.
const Vec3 kWaterShallow { 0.26f, 0.47f, 0.58f };
const Vec3 kWaterDeep { 0.12f, 0.26f, 0.42f };
constexpr f32 kWaterDepthRange = 21.0f; // meters shallow->deep blend

const Vec3 kGrassColor { 0.35f, 0.52f, 0.29f };
const Vec3 kRockColor { 0.47f, 0.44f, 0.41f };
const Vec3 kSnowColor { 0.90f, 0.92f, 0.95f };
const Vec3 kSandColor { 0.76f, 0.69f, 0.51f };

// Hillshade: brightness = base + span * max(dot(normal, light), 0).
const Vec3 kMapLightDir = glm::normalize(Vec3 { -0.45f, 0.80f, -0.40f });
constexpr f32 kShadeBase = 0.62f;
constexpr f32 kShadeSpan = 0.48f;

// Hypsometric bands: every kBandStep meters above sea level lifts the
// tint by kBandLift, capped at kBandLiftMax (subtle terracing).
constexpr f32 kBandStep = 27.0f;
constexpr f32 kBandLift = 0.025f;
constexpr f32 kBandLiftMax = 0.20f;

u8 toByte(f32 channel01) {
    return static_cast<u8>(glm::clamp(channel01, 0.0f, 1.0f) * 255.0f +
                           0.5f);
}

} // namespace

Vec2 mapUv(const MapRasterDesc& desc, f32 worldX, f32 worldZ) {
    const f32 spanX = desc.maxX - desc.minX;
    const f32 spanZ = desc.maxZ - desc.minZ;
    const f32 u = spanX > 0.0f ? (worldX - desc.minX) / spanX : 0.0f;
    const f32 v = spanZ > 0.0f ? (worldZ - desc.minZ) / spanZ : 0.0f;
    return { glm::clamp(u, 0.0f, 1.0f), glm::clamp(v, 0.0f, 1.0f) };
}

vector<u8> generateMapRaster(const MapRasterDesc& desc) {
    const u32 size = desc.size;
    vector<u8> pixels(static_cast<size_t>(size) * size * 4, 0);
    if (!desc.terrain || size == 0) {
        return pixels;
    }
    const render::TerrainParams& params = *desc.terrain;
    const f32 stepX = (desc.maxX - desc.minX) / static_cast<f32>(size);
    const f32 stepZ = (desc.maxZ - desc.minZ) / static_cast<f32>(size);

    // One height evaluation per texel (plus a one-texel apron) instead of
    // the 5 the analytic normal would cost: normals come from central
    // differences over the sampled grid — deterministic, and at map
    // resolution indistinguishable from the analytic ones.
    const u32 grid = size + 2;
    vector<f32> heights(static_cast<size_t>(grid) * grid);
    for (u32 gz = 0; gz < grid; ++gz) {
        const f32 z = desc.minZ + (static_cast<f32>(gz) - 0.5f) * stepZ;
        for (u32 gx = 0; gx < grid; ++gx) {
            const f32 x = desc.minX + (static_cast<f32>(gx) - 0.5f) * stepX;
            heights[static_cast<size_t>(gz) * grid + gx] =
                render::terrain::height(params, x, z);
        }
    }

    const auto heightAt = [&](u32 gx, u32 gz) {
        return heights[static_cast<size_t>(gz) * grid + gx];
    };
    for (u32 pz = 0; pz < size; ++pz) {
        for (u32 px = 0; px < size; ++px) {
            const u32 gx = px + 1; // pixel center in the apron grid
            const u32 gz = pz + 1;
            const f32 h = heightAt(gx, gz);

            Vec3 color;
            if (h < params.seaLevel) {
                const f32 depth = glm::clamp(
                    (params.seaLevel - h) / kWaterDepthRange, 0.0f, 1.0f);
                color = glm::mix(kWaterShallow, kWaterDeep, depth);
            } else {
                // Same convention as render::terrain::normal (Y up,
                // hd - hu on Z), over the grid spacing.
                const Vec3 n = glm::normalize(
                    Vec3 { heightAt(gx - 1, gz) - heightAt(gx + 1, gz),
                           stepX + stepZ, // 2 * step (axes match: square)
                           heightAt(gx, gz - 1) - heightAt(gx, gz + 1) });
                const render::terrain::MaterialWeights w =
                    render::terrain::materialWeights(params, h, n);
                const f32 total = w.grass + w.rock + w.snow + w.sand;
                color = total > 0.0f
                            ? (kGrassColor * w.grass + kRockColor * w.rock +
                               kSnowColor * w.snow + kSandColor * w.sand) /
                                  total
                            : kRockColor;
                const f32 light =
                    glm::max(glm::dot(n, kMapLightDir), 0.0f);
                f32 shade = kShadeBase + kShadeSpan * light;
                const f32 band =
                    std::floor((h - params.seaLevel) / kBandStep);
                shade *= 1.0f + glm::clamp(band * kBandLift, 0.0f,
                                           kBandLiftMax);
                color *= shade;
            }

            const size_t at =
                (static_cast<size_t>(pz) * size + px) * 4;
            pixels[at + 0] = toByte(color.r);
            pixels[at + 1] = toByte(color.g);
            pixels[at + 2] = toByte(color.b);
            pixels[at + 3] = 255;
        }
    }
    return pixels;
}

} // namespace game
