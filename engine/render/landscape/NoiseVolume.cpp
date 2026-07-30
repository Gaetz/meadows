#include "engine/render/landscape/NoiseVolume.hpp"

#include "engine/render/ShaderLibrary.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {
constexpr const char* kBakeShader = "noise_volume";
}

void NoiseVolume::create(rhi::Device& device, ShaderLibrary& shaders) {
    if (!device.caps().computeShaders || !device.caps().volumeTextures) {
        return; // consumers keep the analytic fallback
    }
    shaders.loadCompute(kBakeShader);
    texture = { device, device.createTexture(
        { .width = kSize,
          .height = kSize,
          .depth = kSize,
          .format = rhi::TextureFormat::RGBA8,
          .filter = rhi::FilterMode::Linear,
          .wrap = rhi::AddressMode::Repeat },
        nullptr) };
    sampler = { device, device.createSampler(
        { .addressU = rhi::AddressMode::Repeat,
          .addressV = rhi::AddressMode::Repeat,
          .addressW = rhi::AddressMode::Repeat }) };
    bakeGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 12,
                         .texture = texture.get(),
                         .storageImage = true } } }) };
    sampleGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 9,
                         .texture = texture.get(),
                         .sampler = sampler.get() } } }) };
    bakePipeline = { device, device.createComputePipeline(
        { shaders.get(kBakeShader) }) };
}

void NoiseVolume::destroy(rhi::Device& device) {
    (void)device;
    *this = NoiseVolume {};
}

void NoiseVolume::bakeIfNeeded(rhi::CommandBuffer& cmd) {
    if (baked || bakePipeline.id() == 0) {
        return;
    }
    cmd.setPipeline(bakePipeline);
    cmd.setBindGroup(4, bakeGroup);
    cmd.dispatch(kSize / 8, kSize / 8, kSize / 8);
    cmd.memoryBarrier(rhi::BarrierStage_Fragment);
    baked = true;
}

} // namespace render
