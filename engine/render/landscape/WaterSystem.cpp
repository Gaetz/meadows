#include "engine/render/landscape/WaterSystem.hpp"

#include "engine/core/Jobs.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

void WaterSystem::create(rhi::Device& device, ShaderLibrary& shaders,
                         core::JobSystem& jobSystem) {
    sim.create(jobSystem);
    gpu.create(device, shaders, jobSystem);
}

void WaterSystem::destroy(rhi::Device& device) {
    gpu.destroy(device);
    sim.destroy();
}

void WaterSystem::updateSim(rhi::Device& device, const TerrainParams& params,
                            const Vec3& cameraPos, f32 dt) {
    const auto& requests = sim.update({ .params = &params,
                                        .bodies = gpu.bodiesRef(),
                                        .cameraPos = cameraPos,
                                        .dt = dt });
    gpu.applySimRequests(device, requests, sim.simConfig());
}

} // namespace render
