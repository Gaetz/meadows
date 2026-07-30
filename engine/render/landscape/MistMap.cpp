#include "engine/render/landscape/MistMap.hpp"

#include <cmath>

#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {

// Separable box blur over a kSize² f32 grid, clamped edges, normalized by
// the actual window so borders don't darken. Running-sum: O(n) per row.
void boxBlurAxis(const vector<f32>& src, vector<f32>& dst, u32 size,
                 u32 radius, bool rows) {
    for (u32 line = 0; line < size; ++line) {
        auto at = [&](u32 i) -> f32 {
            return rows ? src[static_cast<size_t>(line) * size + i]
                        : src[static_cast<size_t>(i) * size + line];
        };
        auto out = [&](u32 i) -> f32& {
            return rows ? dst[static_cast<size_t>(line) * size + i]
                        : dst[static_cast<size_t>(i) * size + line];
        };
        f32 sum = 0.0f;
        for (u32 i = 0; i <= radius && i < size; ++i) {
            sum += at(i);
        }
        u32 count = glm::min(radius + 1, size);
        out(0) = sum / static_cast<f32>(count);
        for (u32 i = 1; i < size; ++i) {
            if (i + radius < size) {
                sum += at(i + radius);
                ++count;
            }
            if (i > radius) {
                sum -= at(i - radius - 1);
                --count;
            }
            out(i) = sum / static_cast<f32>(count);
        }
    }
}

} // namespace

void MistMap::create(rhi::Device& device, core::JobSystem& jobSystem) {
    jobs = &jobSystem;
    built = std::make_shared<core::ConcurrentQueue<Baked>>();
    sampler = device.createSampler({});
    // The texture is (re)created per landed bake — the RHI has no texture
    // update, and a rebuild every ~500 m of travel costs nothing.
    (void)device;
}

void MistMap::destroy(rhi::Device& device) {
    ++generation; // orphan in-flight bakes
    device.destroyBindGroup(group);
    device.destroySampler(sampler);
    device.destroyTexture(texture);
    group = {};
    sampler = {};
    texture = {};
    inFlight = false;
    uploaded = false;
}

void MistMap::update(rhi::Device& device, const TerrainParams& params,
                     const Vec3& focus) {
    // 1. Land a finished bake (fresh texture + group).
    Baked done;
    while (built->tryPop(done)) {
        if (done.gen != generation) {
            continue;
        }
        if (texture.id != 0) {
            device.destroyBindGroup(group);
            device.destroyTexture(texture);
        }
        texture = device.createTexture(
            { .width = kSize,
              .height = kSize,
              .format = rhi::TextureFormat::RGBA8,
              .filter = rhi::FilterMode::Linear,
              .usage = rhi::TextureUsage_Sampled },
            done.pixels.data());
        group = device.createBindGroup(
            { .entries = { { .binding = 8,
                             .texture = texture,
                             .sampler = sampler } } });
        center = done.center;
        maxTop = done.maxTop;
        bakedSeed = done.seed;
        bakedSeaLevel = done.seaLevel;
        inFlight = false;
        uploaded = true;
        LOG_INFO("mist map baked: center ({:.0f}, {:.0f}), max top {:.1f} m",
                 center.x, center.y, maxTop);
    }
    if (inFlight) {
        return;
    }

    // 2. Kick a re-bake when the camera leaves the inner quarter of the
    // map or the terrain inputs change (the WaterSystem staleness keys).
    const Vec2 camXz { focus.x, focus.z };
    const bool stale = !uploaded ||
                       glm::distance(camXz, center) > kSpan * 0.25f ||
                       bakedSeed != params.seed ||
                       bakedSeaLevel != params.seaLevel;
    if (!stale) {
        return;
    }
    // Snap the center to the texel grid: texel centers land on the same
    // world lattice every bake, so overlap texels are bit-identical and
    // the swap never pops (the no-crossfade invariant — see header).
    constexpr f32 kTexel = kSpan / static_cast<f32>(kSize);
    const Vec2 want = glm::floor(camXz / kTexel) * kTexel;
    inFlight = true;
    jobs->enqueue([queue = built, params, want, gen = generation] {
        Baked baked;
        baked.center = want;
        baked.seed = params.seed;
        baked.seaLevel = params.seaLevel;
        baked.gen = gen;
        baked.pixels.resize(static_cast<size_t>(kSize) * kSize * 4, 255);

        constexpr f32 texel = kSpan / static_cast<f32>(kSize);
        const f32 originX = want.x - kSpan * 0.5f;
        const f32 originZ = want.y - kSpan * 0.5f;
        vector<f32> heights(static_cast<size_t>(kSize) * kSize);
        for (u32 row = 0; row < kSize; ++row) {
            for (u32 col = 0; col < kSize; ++col) {
                heights[static_cast<size_t>(row) * kSize + col] =
                    terrain::height(
                        params,
                        originX + (static_cast<f32>(col) + 0.5f) * texel,
                        originZ + (static_cast<f32>(row) + 0.5f) * texel);
            }
        }
        // Smoothed height = the surface the mist pools under (~96 m box).
        constexpr u32 kSmoothTexels = 12;
        vector<f32> tmp(heights.size());
        vector<f32> smoothed(heights.size());
        boxBlurAxis(heights, tmp, kSize, kSmoothTexels, true);
        boxBlurAxis(tmp, smoothed, kSize, kSmoothTexels, false);

        const f32 topBase = params.seaLevel + kTopOffset;
        f32 maxTop = topBase;
        for (size_t i = 0; i < heights.size(); ++i) {
            const f32 s = smoothed[i];
            const f32 h = heights[i];
            maxTop = glm::max(maxTop, s);
            u8* px = &baked.pixels[i * 4];
            px[0] = static_cast<u8>(
                glm::clamp((s - topBase) / kTopRange, 0.0f, 1.0f) * 255.0f);
            // No mist banks over open water: an underwater floor reads
            // as a perfect valley, so drop its valleyness — with a
            // short shore fade so lakeside mist still licks the bank.
            const f32 shore =
                glm::smoothstep(params.seaLevel - 2.0f,
                                params.seaLevel + 3.0f, h);
            px[1] = static_cast<u8>(
                glm::clamp((s - h) / kValleyDepth, 0.0f, 1.0f) * shore *
                255.0f);
            px[2] = 0;
            px[3] = 255;
        }
        baked.maxTop = maxTop;
        queue->push(std::move(baked));
    });
}

} // namespace render
