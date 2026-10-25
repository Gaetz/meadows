#include "engine/render/landscape/TerrainShadeMap.hpp"

#include <cmath>
#include <utility>

#include "engine/core/Jobs.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

void TerrainShadeMap::create(rhi::Device& device, core::JobSystem& jobSystem) {
    mailbox.create(jobSystem, "terrainShadeMap");
    sampler = { device, device.createSampler({}) };
    // Textures are (re)created per landed bake (no RHI texture update).
}

void TerrainShadeMap::destroy(rhi::Device& device) {
    (void)device;
    // Orphan in-flight bakes: the mailbox generation must keep moving
    // ACROSS the reset (a fresh mailbox would restart it at zero).
    core::BakeMailbox<Baked> saved = std::move(mailbox);
    saved.reset();
    *this = TerrainShadeMap {}; // the owning handles release themselves
    mailbox = std::move(saved);
}

void TerrainShadeMap::update(rhi::Device& device, const TerrainParams& params,
                             const Vec3& focus) {
    mailbox.drain([&](Baked& done) {
        // Re-assignment releases the previous bake's textures and group.
        const rhi::TextureDesc desc { .width = kSize,
                                      .height = kSize,
                                      .format = rhi::TextureFormat::RGBA8,
                                      .filter = rhi::FilterMode::Linear,
                                      .usage = rhi::TextureUsage_Sampled };
        texture0 = { device, device.createTexture(desc, done.t0.data()) };
        texture1 = { device, device.createTexture(desc, done.t1.data()) };
        group = { device,
                  device.createBindGroup(
                      { .entries = { { .binding = 4,
                                       .texture = texture0,
                                       .sampler = sampler },
                                     { .binding = 5,
                                       .texture = texture1,
                                       .sampler = sampler } } }) };
        center = done.center;
        bakedStamp = done.stamp;
    });
    if (mailbox.busy()) {
        return;
    }

    const Vec2 want { focus.x, focus.z };
    const bool strayed = mailbox.strayed(center, want, kSpan * 0.25f);
    const bool contentChanged =
        mailbox.ready() &&
        terrain::contentTouchedSince(params, bakedStamp,
                                     center.x - kSpan * 0.5f,
                                     center.y - kSpan * 0.5f,
                                     center.x + kSpan * 0.5f,
                                     center.y + kSpan * 0.5f);
    if (!strayed && !contentChanged) {
        return;
    }
    mailbox.kick([params, want](Baked& baked,
                                const std::atomic<bool>& stop) {
        baked.center = want;
        baked.stamp = params.contentStamp;
        baked.t0.resize(static_cast<size_t>(kSize) * kSize * 4);
        baked.t1.resize(static_cast<size_t>(kSize) * kSize * 4);
        const f32 texel = kSpan / static_cast<f32>(kSize);
        const f32 originX = want.x - kSpan * 0.5f;
        const f32 originZ = want.y - kSpan * 0.5f;
        const auto toByte = [](f32 v) {
            return static_cast<u8>(glm::clamp(v, 0.0f, 1.0f) * 255.0f +
                                   0.5f);
        };
        for (u32 row = 0; row < kSize; ++row) {
            if (stop.load(std::memory_order_relaxed)) {
                return; // shutdown: the mailbox drops the partial bake
            }
            for (u32 col = 0; col < kSize; ++col) {
                const f32 x =
                    originX + (static_cast<f32>(col) + 0.5f) * texel;
                const f32 z =
                    originZ + (static_cast<f32>(row) + 0.5f) * texel;
                const terrain::RegionShading shading =
                    terrain::regionShadingAt(params, x, z);
                const size_t at =
                    (static_cast<size_t>(row) * kSize + col) * 4;
                baked.t0[at + 0] = toByte(shading.tint.r);
                baked.t0[at + 1] = toByte(shading.tint.g);
                baked.t0[at + 2] = toByte(shading.tint.b);
                baked.t0[at + 3] = toByte(shading.fields.wetness);
                baked.t1[at + 0] = toByte(shading.fields.rockiness);
                // Snow-line offset: (byte - 128) * 8 m on decode.
                baked.t1[at + 1] = static_cast<u8>(glm::clamp(
                    std::round(shading.fields.snowLineOffset / 8.0f) +
                        128.0f,
                    0.0f, 255.0f));
                baked.t1[at + 2] = toByte(shading.fields.sandiness);
                baked.t1[at + 3] = toByte(shading.fields.beach);
            }
        }
    });
}

} // namespace render
