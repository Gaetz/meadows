#include "engine/render/landscape/FireScorchMap.hpp"

#include "engine/rhi/Device.hpp"

namespace render {

namespace {
constexpr u32 kScorchBinding = 10; // firescorch.glsl
} // namespace

void FireScorchMap::create(rhi::Device& device) {
    sampler = device.createSampler({}); // linear clamp: soft cell edges
    const f32 kNone = 0.0f;
    texture = device.createTexture({ .width = 1,
                                     .height = 1,
                                     .format = rhi::TextureFormat::R16F,
                                     .usage = rhi::TextureUsage_Sampled },
                                   &kNone);
    rebuildGroup(device);
}

void FireScorchMap::destroy(rhi::Device& device) {
    if (group.id != 0) {
        device.destroyBindGroup(group);
    }
    if (texture.id != 0) {
        device.destroyTexture(texture);
    }
    if (sampler.id != 0) {
        device.destroySampler(sampler);
    }
    group = {};
    texture = {};
    sampler = {};
    cells = 0;
    texel = 0.0f;
    staging.clear();
}

void FireScorchMap::rebuildGroup(rhi::Device& device) {
    if (group.id != 0) {
        device.destroyBindGroup(group);
    }
    group = device.createBindGroup(
        { .entries = { { .binding = kScorchBinding,
                         .texture = texture,
                         .sampler = sampler } } });
}

void FireScorchMap::upload(rhi::Device& device, const vector<u8>& scorch,
                           u32 n, const Vec2& at, f32 cellSize) {
    if (n == 0 || scorch.size() < static_cast<size_t>(n) * n) {
        clear(device);
        return;
    }
    staging.resize(static_cast<size_t>(n) * n);
    for (size_t i = 0; i < staging.size(); ++i) {
        staging[i] = static_cast<f32>(scorch[i]) * (1.0f / 255.0f);
    }
    if (texture.id != 0) {
        device.destroyTexture(texture);
    }
    texture = device.createTexture({ .width = n,
                                     .height = n,
                                     .format = rhi::TextureFormat::R16F,
                                     .usage = rhi::TextureUsage_Sampled },
                                   staging.data());
    origin = at;
    texel = cellSize;
    cells = n;
    rebuildGroup(device);
}

void FireScorchMap::clear(rhi::Device& device) {
    if (cells == 0) {
        return;
    }
    if (texture.id != 0) {
        device.destroyTexture(texture);
    }
    const f32 kNone = 0.0f;
    texture = device.createTexture({ .width = 1,
                                     .height = 1,
                                     .format = rhi::TextureFormat::R16F,
                                     .usage = rhi::TextureUsage_Sampled },
                                   &kNone);
    cells = 0;
    texel = 0.0f;
    rebuildGroup(device);
}

} // namespace render
