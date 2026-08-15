#include "engine/render/landscape/TerrainShadeMap.hpp"

#include <cmath>

#include "engine/core/Jobs.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

void TerrainShadeMap::create(rhi::Device& device, core::JobSystem& jobSystem) {
    mailbox.create(jobSystem);
    sampler = device.createSampler({});
    // Textures are (re)created per landed bake (no RHI texture update).
    (void)device;
}

void TerrainShadeMap::destroy(rhi::Device& device) {
    mailbox.reset(); // orphan in-flight bakes
    device.destroyBindGroup(group);
    device.destroySampler(sampler);
    device.destroyTexture(texture0);
    device.destroyTexture(texture1);
    group = {};
    sampler = {};
    texture0 = {};
    texture1 = {};
}

void TerrainShadeMap::update(rhi::Device& device, const TerrainParams& params,
                             const Vec3& focus) {
    mailbox.drain([&](Baked& done) {
        if (texture0.id != 0) {
            device.destroyBindGroup(group);
            device.destroyTexture(texture0);
            device.destroyTexture(texture1);
        }
        const rhi::TextureDesc desc { .width = kSize,
                                      .height = kSize,
                                      .format = rhi::TextureFormat::RGBA8,
                                      .filter = rhi::FilterMode::Linear,
                                      .usage = rhi::TextureUsage_Sampled };
        texture0 = device.createTexture(desc, done.t0.data());
        texture1 = device.createTexture(desc, done.t1.data());
        group = device.createBindGroup(
            { .entries = { { .binding = 4,
                             .texture = texture0,
                             .sampler = sampler },
                           { .binding = 5,
                             .texture = texture1,
                             .sampler = sampler } } });
        center = done.center;
        bakedStamp = done.stamp;
    });
    if (mailbox.busy()) {
        return;
    }

    const Vec2 want { focus.x, focus.z };
    const bool strayed = !mailbox.ready() ||
                         glm::distance(want, center) > kSpan * 0.25f;
    const bool contentChanged =
        mailbox.ready() && bakedStamp != params.contentStamp;
    if (!strayed && !contentChanged) {
        return;
    }
    mailbox.kick([params, want](Baked& baked) {
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
