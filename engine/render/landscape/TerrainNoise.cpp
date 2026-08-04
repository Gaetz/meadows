#include "engine/render/landscape/TerrainNoise.hpp"

#include <cmath>

#include "engine/core/Hash.hpp"

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

bool underLocalWater(const TerrainParams& params, f32 x, f32 z, f32 h,
                     f32 margin) {
    return params.water &&
           waterDepthAt(*params.water, x, z, h - margin) > 0.0f;
}

f32 rockExposureAt(const TerrainParams& params, f32 x, f32 z) {
    const TerrainRegion* region =
        params.base ? params.base->regionAt(x, z) : nullptr;
    if (!region || region->rockExposure.empty()) {
        return 0.0f;
    }
    return maskSample(*region, region->rockExposure, x, z, 0.0f);
}

namespace {

// The snow/sand border wander — the CPU mirror of terrain_weights.glsl's
// borderWander (same murmur finalizer, same lattice, same -0.31 bias).
// Material-set independent by design: the old grass-albedo green tap
// broke the calibration when cooked photo tiles replaced the procedural
// ones.
f32 wanderLattice(i32 x, i32 y) {
    const u32 h = core::hashU32(static_cast<u32>(x) * 0x9e3779b9u ^
                                static_cast<u32>(y) * 0x85ebca6bu);
    return static_cast<f32>(h) * (1.0f / 4294967295.0f);
}

f32 borderWander(f32 px, f32 py) {
    const f32 fx = std::floor(px);
    const f32 fy = std::floor(py);
    const f32 tx = px - fx;
    const f32 ty = py - fy;
    const f32 ux = tx * tx * (3.0f - 2.0f * tx);
    const f32 uy = ty * ty * (3.0f - 2.0f * ty);
    const i32 x0 = static_cast<i32>(fx);
    const i32 y0 = static_cast<i32>(fy);
    const f32 v00 = wanderLattice(x0, y0);
    const f32 v10 = wanderLattice(x0 + 1, y0);
    const f32 v01 = wanderLattice(x0, y0 + 1);
    const f32 v11 = wanderLattice(x0 + 1, y0 + 1);
    const f32 n = glm::mix(glm::mix(v00, v10, ux), glm::mix(v01, v11, ux),
                           uy);
    return -0.31f + (n - 0.5f) * 0.2f;
}

// terrain_zones.glsl mirror — see GrassZone in the header.
constexpr f32 kGrassZoneSize = 3.0f;

// THE weight rule, in one place — shaders/terrain_weights.glsl mirrors it
// bit-for-bit and every CPU consumer (GI/minimap, scatter, footsteps) goes
// through it. Neutral inputs reproduce the shader exactly; the biome-aware
// caller feeds rockShift/snowLineOffset (via snowLine)/sandiness/beach/
// grassPresence. The hybrid-C seam: region shading extends these INPUTS
// and a future painted override composes on the RESULT — neither rewrites
// the rule.
struct WeightRuleInputs {
    f32 slope { 0.0f };
    f32 height { 0.0f };
    f32 wander { 0.0f };       // shader-visible altitude perturbation
    f32 rockExposure { 0.0f };
    f32 snowLine { 0.0f };     // biome offset already applied by the caller
    f32 seaLevel { 0.0f };
    f32 rockShift { 0.0f };    // 0.1 * biome.rockiness
    f32 sandiness { 0.0f };
    f32 beach { 0.0f };        // baked coast mask forces sand
    f32 grassPresence { 1.0f };
};

MaterialWeights materialWeightsCore(const WeightRuleInputs& in) {
    MaterialWeights weights;
    weights.cliff =
        glm::smoothstep(0.30f, 0.55f, in.slope) * in.rockExposure;
    weights.rock =
        glm::smoothstep(0.18f - in.rockShift, 0.35f - in.rockShift,
                        in.slope) *
        (1.0f - weights.cliff);
    weights.snow = glm::smoothstep(in.snowLine - 12.0f, in.snowLine + 42.0f,
                                   in.height + in.wander * 26.0f) *
                   (1.0f - glm::smoothstep(0.25f, 0.45f, in.slope));
    weights.sand =
        (1.0f - glm::smoothstep(in.seaLevel + 1.0f + 6.0f * in.sandiness,
                                in.seaLevel + 8.0f + 24.0f * in.sandiness,
                                in.height + in.wander * 5.0f)) *
        (1.0f - weights.rock - weights.cliff);
    weights.sand = glm::max(weights.sand,
                            in.beach * (1.0f - weights.rock - weights.cliff));
    weights.grass = glm::max(1.0f - weights.rock - weights.snow -
                                 weights.sand - weights.cliff,
                             0.0f) *
                    in.grassPresence;
    return weights;
}

} // namespace

MaterialWeights materialWeights(const TerrainParams& params, f32 height,
                                const Vec3& normal) {
    // No (x, z) here, so no exposure mask: cliff stays 0 and the
    // steepest faces read as rock — fine for the GI/minimap consumers.
    return materialWeightsCore({ .slope = 1.0f - normal.y,
                                 .height = height,
                                 .snowLine = params.snowLine,
                                 .seaLevel = params.seaLevel });
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

GrassZone grassZoneAt(f32 x, f32 z) {
    // Hex-tiling mirror (terrain_zones.glsl hexGrass): the dominant
    // triangle-lattice vertex — same skew, same hash, same sharpening —
    // so species/clutter biases follow what the ground shows.
    const f32 qx = x / kGrassZoneSize;
    const f32 qy = z / kGrassZoneSize;
    const f32 sx = qx - qy * 0.57735027f;
    const f32 sy = qy * 1.15470054f;
    const i32 baseX = static_cast<i32>(std::floor(sx));
    const i32 baseY = static_cast<i32>(std::floor(sy));
    const f32 fx = sx - static_cast<f32>(baseX);
    const f32 fy = sy - static_cast<f32>(baseY);
    i32 vx[3];
    i32 vy[3];
    f32 w[3];
    if (fx + fy < 1.0f) {
        vx[0] = baseX;
        vy[0] = baseY;
        vx[1] = baseX + 1;
        vy[1] = baseY;
        vx[2] = baseX;
        vy[2] = baseY + 1;
        w[0] = 1.0f - fx - fy;
        w[1] = fx;
        w[2] = fy;
    } else {
        vx[0] = baseX + 1;
        vy[0] = baseY + 1;
        vx[1] = baseX;
        vy[1] = baseY + 1;
        vx[2] = baseX + 1;
        vy[2] = baseY;
        w[0] = fx + fy - 1.0f;
        w[1] = 1.0f - fx;
        w[2] = 1.0f - fy;
    }
    f32 sum = 0.0f;
    for (f32& wi : w) {
        wi = std::pow(wi, 6.0f); // kHexSharpness
        sum += wi;
    }
    u32 dom = 0;
    u32 second = 1;
    if (w[1] > w[dom]) {
        dom = 1;
        second = 0;
    }
    if (w[2] > w[dom]) {
        second = dom;
        dom = 2;
    } else if (w[2] > w[second]) {
        second = 2;
    }
    const auto variantOf = [](i32 ix, i32 iy) {
        return core::hashU32(static_cast<u32>(ix) * 0x9e3779b9u ^
                             static_cast<u32>(iy) * 0x85ebca6bu) &
               3u;
    };
    GrassZone zone;
    zone.variantA = variantOf(vx[dom], vy[dom]);
    zone.variantB = variantOf(vx[second], vy[second]);
    zone.blendA = sum > 0.0f ? w[dom] / sum : 1.0f;
    return zone;
}

RegionFields regionFieldsAt(const TerrainParams& params, f32 x, f32 z) {
    const BiomeParams& biome = biomeAt(params, x, z);
    const TerrainRegion* region =
        params.base ? params.base->regionAt(x, z) : nullptr;
    RegionFields fields;
    fields.rockiness = biome.rockiness;
    fields.snowLineOffset = biome.snowLineOffset;
    fields.sandiness = biome.sandiness;
    fields.grassPresence = biome.grassPresence;
    fields.temperature = biome.temperature;
    fields.biomeWetness = biome.wetness;
    if (region) {
        fields.wetness = maskSample(*region, region->wetness, x, z, 0.0f);
        // The baked beach mask forces sand where the coast pass decided
        // so, whatever the altitude rules say.
        fields.beach = maskSample(*region, region->beach, x, z, 0.0f);
    }
    return fields;
}

RegionShading regionShadingAt(const TerrainParams& params, f32 x, f32 z) {
    RegionShading shading;
    shading.fields = regionFieldsAt(params, x, z);
    // Macro tint: biome climate resolved to a color multiplier, modulated
    // by a ~700 m aridity drift so plains breathe between lush and parched
    // instead of tracing biome borders — the repetition killer of the
    // texturing brief (low-frequency COLOR variation). Ground and grass
    // and GI all consume this one tint; the strength knob lives shader-
    // side (uSplatDetailInfo.y), the map stores the full-strength color.
    // The fbm is why this stays OFF the scatter/footstep hot path
    // (regionFieldsAt) — bakes and sparse corner lattices only.
    const f32 drift = fbm(params.seed ^ 0x7e4a1c3du, x, z, 1.0f / 700.0f,
                          3, 2.0f, 0.5f);
    const f32 aridity = glm::clamp(0.5f + 0.35f * shading.fields.temperature +
                                       (drift - 0.5f) * 0.8f -
                                       0.4f * shading.fields.biomeWetness,
                                   0.0f, 1.0f);
    const Vec3 lush { 0.92f, 1.02f, 0.94f };
    const Vec3 parched { 1.08f, 1.00f, 0.82f };
    Vec3 tint = glm::mix(lush, parched, aridity);
    // Cold climates pale toward blue-gray; moisture (river/lake bands)
    // darkens the ground — the "wet earth" cue near water.
    tint = glm::mix(tint, Vec3 { 0.96f, 0.99f, 1.05f },
                    glm::clamp(-shading.fields.temperature, 0.0f, 1.0f) *
                        0.5f);
    tint *= 1.0f - 0.18f * shading.fields.wetness;
    shading.tint = tint;
    return shading;
}

MaterialWeights materialWeightsAt(const TerrainParams& params, f32 x,
                                  f32 z, f32 height, const Vec3& normal) {
    const RegionFields fields = regionFieldsAt(params, x, z);
    return materialWeightsCore(
        { .slope = 1.0f - normal.y,
          .height = height,
          .rockExposure = rockExposureAt(params, x, z),
          .snowLine = params.snowLine + fields.snowLineOffset,
          .seaLevel = params.seaLevel,
          .rockShift = 0.1f * fields.rockiness,
          .sandiness = fields.sandiness,
          .beach = fields.beach,
          .grassPresence = fields.grassPresence });
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
    const f32 wander = borderWander(x * splatUvScale * 0.06f,
                                    z * splatUvScale * 0.06f);
    // Region fields (biome rules, beach) mirror the shader's shade-map
    // taps: the step keeps sounding like the ground LOOKS. grassPresence
    // stays scatter-only (the shader renormalizes it away).
    const RegionFields fields = regionFieldsAt(params, x, z);
    return materialWeightsCore(
        { .slope = 1.0f - n.y,
          .height = h,
          .wander = wander,
          .rockExposure = rockExposureAt(params, x, z),
          .snowLine = params.snowLine + fields.snowLineOffset,
          .seaLevel = params.seaLevel,
          .rockShift = 0.1f * fields.rockiness,
          .sandiness = fields.sandiness,
          .beach = fields.beach });
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
