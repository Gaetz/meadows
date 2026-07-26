#include "engine/render/landscape/LightClusters.hpp"

#include "engine/render/ShaderLibrary.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {
constexpr const char* kCullShader = "cluster_cull";
}

void LightClusters::createBuffer(rhi::Device& device) {
    buffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Storage,
          .size = kClusterX * kClusterY * kClusterZ * kSlots * sizeof(u32) },
        nullptr);
}

void LightClusters::createPipeline(rhi::Device& device,
                                   ShaderLibrary& shaders) {
    shaders.loadCompute(kCullShader,
                        { { "FrameUbo", 0 }, { "LightsUbo", 5 } });
    refreshPipeline(device, shaders);
}

void LightClusters::refreshPipeline(rhi::Device& device,
                                    ShaderLibrary& shaders) {
    if (shaders.generation(kCullShader) == generation) {
        return;
    }
    if (pipeline.id != 0) {
        device.destroyPipeline(pipeline);
    }
    pipeline = device.createComputePipeline({ shaders.get(kCullShader) });
    generation = shaders.generation(kCullShader);
}

void LightClusters::destroy(rhi::Device& device) {
    device.destroyPipeline(pipeline);
    device.destroyBuffer(buffer);
    *this = LightClusters {};
}

void LightClusters::run(rhi::CommandBuffer& cmd) {
    cmd.setPipeline(pipeline);
    cmd.dispatch((kClusterX + 7) / 8, (kClusterY + 7) / 8, kClusterZ);
    // The lists feed the surface shaders (fragment) and the froxel
    // inject (compute) — the raster passes recorded in between (CSM,
    // key shadows, cloud bake) read nothing of them and may overlap.
    cmd.memoryBarrier(rhi::BarrierDst_Fragment | rhi::BarrierDst_Compute);
}

} // namespace render
