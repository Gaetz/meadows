#include "engine/render/landscape/TerrainNoise.hpp"

#include "engine/render/landscape/SplatTextures.hpp" // splatWander

#include <cmath>

#include "engine/terrain/Noise.hpp"
#include "engine/terrain/SandboxTerrain.hpp"

namespace render {

namespace {

// The primitives live in engine/terrain/Noise.hpp (headless, shared with
// the terrain generator); these aliases keep the composition below
// readable.
using noise::fbm;
using noise::ridgedFbm;
using noise::smoothstep01;
constexpr auto valueNoise = noise::value;

f32 smoothstepf(f32 low, f32 high, f32 x) {
    return smoothstep01(low, high, x);
}

// The procedural fallback outside baked regions (and their blend
// partner in edge bands): the analytic sandbox macro when a sandbox
// identity is set, else the legacy demo noise — bit-identical to the
// pre-baked-terrain behavior in that case.
f32 proceduralBase(const render::TerrainParams& params, f32 x, f32 z) {
    if (params.sandbox) {
        const render::terraingen::ProceduralControls controls {
            params.sandbox->controls
        };
        return render::terraingen::macroHeightAnalytic(
            controls, params.sandbox->macro, x, z);
    }
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

// Runtime detail on top of a baked macro grid: frequencies below the grid's
// Nyquist so bake and detail never fight. Zero-amplitude regions (debug
// bakes of the procedural terrain) skip it entirely.
f32 detailNoise(const render::TerrainParams& params,
                const render::TerrainRegion& region, f32 x, f32 z) {
    if (region.detailAmplitude <= 0.0f || region.detailOctaves <= 0) {
        return 0.0f;
    }
    const f32 amp = region.detailAmplitude *
                    render::terrain::maskSample(region, region.detailAmp, x,
                                                z, 1.0f);
    return (fbm(params.seed ^ 0x7f4a7c15u, x, z,
                1.0f / region.detailWavelength, region.detailOctaves,
                params.lacunarity, params.gain) *
                2.0f -
            1.0f) *
           amp;
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
    weights.snow = glm::smoothstep(params.snowLine - 12.0f,
                                   params.snowLine + 42.0f,
                                   height) *
                   (1.0f - glm::smoothstep(0.25f, 0.45f, slope));
    weights.sand = (1.0f - glm::smoothstep(params.seaLevel + 1.0f,
                                           params.seaLevel + 8.0f, height)) *
                   (1.0f - weights.rock);
    weights.grass = glm::max(
        1.0f - weights.rock - weights.snow - weights.sand, 0.0f);
    return weights;
}

const BiomeParams& biomeAt(const TerrainParams& params, f32 x, f32 z) {
    static const BiomeParams kNeutral {};
    if (!params.biomes || params.biomes->table.empty()) {
        return kNeutral;
    }
    u8 index = 0;
    const TerrainRegion* region =
        params.base ? params.base->regionAt(x, z) : nullptr;
    if (region && !region->biome.empty() && region->maskWidth >= 2) {
        // Nearest texel of the region's biome channel (ids don't blend).
        const f32 texel =
            region->spanX() / static_cast<f32>(region->maskWidth - 1);
        const u32 cx = static_cast<u32>(glm::clamp(
            (x - region->originX) / texel + 0.5f, 0.0f,
            static_cast<f32>(region->maskWidth - 1)));
        const f32 texelZ =
            region->spanZ() / static_cast<f32>(region->maskHeight - 1);
        const u32 cz = static_cast<u32>(glm::clamp(
            (z - region->originZ) / texelZ + 0.5f, 0.0f,
            static_cast<f32>(region->maskHeight - 1)));
        index = region->biome[static_cast<size_t>(cz) *
                                  region->maskWidth +
                              cx];
    } else {
        index = params.biomes->paintedIndexAt(x, z);
    }
    const size_t clamped =
        glm::min<size_t>(index, params.biomes->table.size() - 1);
    return params.biomes->table[clamped];
}

MaterialWeights materialWeightsAt(const TerrainParams& params, f32 x,
                                  f32 z, f32 height, const Vec3& normal) {
    const BiomeParams& biome = biomeAt(params, x, z);
    const f32 slope = 1.0f - normal.y;
    const f32 rockShift = 0.1f * biome.rockiness;
    const f32 snowLine = params.snowLine + biome.snowLineOffset;
    MaterialWeights weights;
    weights.rock =
        glm::smoothstep(0.18f - rockShift, 0.35f - rockShift, slope);
    weights.snow = glm::smoothstep(snowLine - 12.0f, snowLine + 42.0f,
                                   height) *
                   (1.0f - glm::smoothstep(0.25f, 0.45f, slope));
    weights.sand =
        (1.0f - glm::smoothstep(
                    params.seaLevel + 1.0f + 6.0f * biome.sandiness,
                    params.seaLevel + 8.0f + 24.0f * biome.sandiness,
                    height)) *
        (1.0f - weights.rock);
    // The baked beach mask forces sand where the coast pass decided so,
    // whatever the altitude rules say.
    const TerrainRegion* region =
        params.base ? params.base->regionAt(x, z) : nullptr;
    if (region) {
        const f32 beach =
            maskSample(*region, region->beach, x, z, 0.0f);
        weights.sand =
            glm::max(weights.sand, beach * (1.0f - weights.rock));
    }
    weights.grass =
        glm::max(1.0f - weights.rock - weights.snow - weights.sand,
                 0.0f) *
        biome.grassPresence;
    return weights;
}

MaterialWeights materialWeightsShaded(const TerrainParams& params, f32 x,
                                      f32 z, f32 splatUvScale) {
    // Keep in LOCKSTEP with terrain.frag: uv = xz * scale; wander =
    // splat green at uv * 0.06 - 0.67 (tile mean green 0.36 linear plus
    // the -0.31 bias the snow/sand lines are tuned against); the
    // snow/sand altitudes shift by wander * 26 / * 5 before the same
    // smoothsteps.
    const f32 h = height(params, x, z);
    const Vec3 n = normal(params, x, z);
    const f32 slope = 1.0f - n.y;
    const f32 u = x * splatUvScale * 0.06f;
    const f32 v = z * splatUvScale * 0.06f;
    const f32 wander =
        splatWander(u - std::floor(u), v - std::floor(v)) - 0.67f;
    MaterialWeights weights;
    weights.rock = glm::smoothstep(0.18f, 0.35f, slope);
    weights.snow = glm::smoothstep(params.snowLine - 12.0f,
                                   params.snowLine + 42.0f,
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
    // Baked regions blend by edge weight — overlapping sandbox tiles
    // merge inside their shared margin ring; where total coverage fades,
    // the procedural fallback blends back in. Fully-covered samples never
    // pay the procedural evaluation.
    f32 wSum = 0.0f;
    f32 hSum = 0.0f;
    if (params.base) {
        for (const TerrainRegion& region : params.base->regions) {
            if (!region.contains(x, z)) {
                continue;
            }
            const f32 w = edgeWeight(region, x, z);
            if (w <= 0.0f) {
                continue;
            }
            hSum += w * (baseHeight(region, x, z) +
                         detailNoise(params, region, x, z));
            wSum += w;
        }
    }
    f32 base;
    if (wSum >= 1.0f) {
        base = hSum / wSum;
    } else if (wSum > 0.0f) {
        base = glm::mix(proceduralBase(params, x, z), hSum / wSum, wSum);
    } else {
        base = proceduralBase(params, x, z);
    }
    return params.patches ? base + authoredDelta(*params.patches, x, z)
                          : base;
}

TerrainRegion bakeProceduralRegion(const TerrainParams& params, f32 originX,
                                   f32 originZ, f32 sizeMeters,
                                   f32 texelSize) {
    TerrainRegion region;
    region.originX = originX;
    region.originZ = originZ;
    region.texelSize = texelSize;
    const u32 n =
        static_cast<u32>(std::lround(sizeMeters / texelSize)) + 1u;
    region.width = n;
    region.height = n;
    region.heights.resize(static_cast<size_t>(n) * n);
    for (u32 row = 0; row < n; ++row) {
        const f32 z = originZ + static_cast<f32>(row) * texelSize;
        for (u32 col = 0; col < n; ++col) {
            const f32 x = originX + static_cast<f32>(col) * texelSize;
            region.heights[static_cast<size_t>(row) * n + col] =
                proceduralBase(params, x, z);
        }
    }
    return region;
}

f32 meshHeight(const TerrainParams& params, f32 x, f32 z, f32 spacing) {
    f32 h = height(params, x, z);
    if (spacing <= 2.5f || !params.base) {
        return h;
    }
    const TerrainRegion* region = params.base->regionAt(x, z);
    if (!region || region->flow.empty()) {
        return h;
    }
    if (maskSample(*region, region->flow, x, z, 0.0f) < 0.5f) {
        return h;
    }
    const f32 r = spacing * 0.5f;
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dz == 0) {
                continue;
            }
            h = glm::min(h, height(params, x + static_cast<f32>(dx) * r,
                                   z + static_cast<f32>(dz) * r));
        }
    }
    return h;
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
