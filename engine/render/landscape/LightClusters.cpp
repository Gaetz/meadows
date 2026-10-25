#include "engine/render/landscape/LightClusters.hpp"

#include "engine/render/ShaderLibrary.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"
#include "engine/render/PipelinePresets.hpp"

namespace render {

namespace {
constexpr const char* kCullShader = "cluster_cull";
}

void LightClusters::createBuffer(rhi::Device& device) {
    buffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Storage,
          .size = kClusterX * kClusterY * kClusterZ * kSlots * sizeof(u32) },
        nullptr) };
}

void LightClusters::createPipeline(rhi::Device& device,
                                   ShaderLibrary& shaders) {
    shaders.loadCompute(kCullShader,
                        { render::kFrameUbo, render::kLightsUbo });
    refreshPipeline(device, shaders);
}

void LightClusters::refreshPipeline(rhi::Device& device,
                                    ShaderLibrary& shaders) {
    pipeline.ensure(device, shaders, [&] {
        return device.createComputePipeline({ shaders.get(kCullShader) });
    });
}

void LightClusters::destroy(rhi::Device& device) {
    (void)device; // Unique handles free through their device
    *this = LightClusters {};
}

void LightClusters::run(rhi::CommandBuffer& cmd) {
    cmd.setPipeline(pipeline);
    cmd.dispatch((kClusterX + 7) / 8, (kClusterY + 7) / 8, kClusterZ);
    // The lists feed the surface shaders (fragment) and the froxel
    // inject (compute) — the raster passes recorded in between (CSM,
    // key shadows, cloud bake) read nothing of them and may overlap.
    cmd.memoryBarrier(rhi::BarrierStage_Fragment | rhi::BarrierStage_Compute);
}

} // namespace render
