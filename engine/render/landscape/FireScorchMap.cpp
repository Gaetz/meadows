#include "engine/render/landscape/FireScorchMap.hpp"

#include "engine/rhi/Device.hpp"

namespace render {

namespace {
constexpr u32 kScorchBinding = 10; // firescorch.glsl
} // namespace

void FireScorchMap::create(rhi::Device& device) {
    sampler = device.createSampler({}); // linear clamp: soft cell edges
    const u8 kNone[4] = { 0, 0, 0, 255 };
    texture = device.createTexture({ .width = 1,
                                     .height = 1,
                                     .format = rhi::TextureFormat::RGBA8,
                                     .usage = rhi::TextureUsage_Sampled },
                                   kNone);
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
                           const vector<u8>& glow, const vector<u8>& canopy,
                           u32 n, const Vec2& at, f32 cellSize) {
    const size_t count = static_cast<size_t>(n) * n;
    if (n == 0 || scorch.size() < count) {
        clear(device);
        return;
    }
    const bool hasGlow = glow.size() >= count;
    const bool hasCanopy = canopy.size() >= count;
    staging.resize(count * 4);
    for (size_t i = 0; i < count; ++i) {
        staging[i * 4 + 0] = scorch[i];
        staging[i * 4 + 1] = hasGlow ? glow[i] : 0;
        staging[i * 4 + 2] = hasCanopy ? canopy[i] : 0;
        staging[i * 4 + 3] = 255;
    }
    if (texture.id != 0) {
        device.destroyTexture(texture);
    }
    texture = device.createTexture({ .width = n,
                                     .height = n,
                                     .format = rhi::TextureFormat::RGBA8,
                                     .filter = rhi::FilterMode::Linear,
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
    const u8 kNone[4] = { 0, 0, 0, 255 };
    texture = device.createTexture({ .width = 1,
                                     .height = 1,
                                     .format = rhi::TextureFormat::RGBA8,
                                     .usage = rhi::TextureUsage_Sampled },
                                   kNone);
    cells = 0;
    texel = 0.0f;
    rebuildGroup(device);
}

} // namespace render
