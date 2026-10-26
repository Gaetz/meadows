#pragma once

#include <glm/glm.hpp>

#include <array>
#include <functional>
#include <string>

#include "engine/core/BakeMailbox.hpp"
#include "engine/core/Clock.hpp"
#include "engine/core/Defines.hpp"
#include "engine/render/landscape/HeightField.hpp"
#include "engine/render/landscape/WaterRenderer.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/render/WatchedPipeline.hpp"
#include "engine/rhi/Rhi.hpp"
#include "engine/rhi/UniqueHandle.hpp"
#include "engine/terrain/TerrainNoise.hpp"
#include "engine/terrain/WaterBodies.hpp"
#include "engine/terrain/WaterGeometry.hpp"
#include "engine/terrain/WaterSim.hpp"
#include "engine/terrain/WaterSimControl.hpp"

namespace core {
class JobSystem;
}
namespace rhi {
class CommandBuffer;
class Device;
}

namespace render {

class ShaderLibrary;

// The water as ONE object for the orchestrator and the game: a thin
// composition of the headless sim owner (terrain::WaterSimControl — the
// phase machine, the worker lane, the snapshot the queries read) and the
// GPU half (WaterRenderer — pipelines, textures, meshes, draws). Every
// method here forwards to one of the two; the sim-facing ones are the
// façade the game drives until it is routed to simControl() directly.
class WaterSystem {
public:
    static constexpr u32 kPoolMapSize = WaterRenderer::kPoolMapSize;
    static constexpr f32 kPoolMapSpan = WaterRenderer::kPoolMapSpan;
    static constexpr f32 kRebakeDistance = WaterRenderer::kRebakeDistance;
    // Water material presets: mirrored to GLSL by the frame constants
    // preamble (FrameLanes.hpp) under this name.
    static constexpr u32 kMaxWaterMaterials = terrain::kMaxWaterMaterials;

    using FarWaterSet = terrain::FarWaterSet;
    using FarWaterFn = WaterRenderer::FarWaterFn;
    using SimConfig = terrain::WaterSimControl::SimConfig;
    using SimSourcesFn = terrain::WaterSimControl::SimSourcesFn;

    // The two halves, for callers that address one directly.
    terrain::WaterSimControl& simControl() { return sim; }
    const terrain::WaterSimControl& simControl() const { return sim; }
    WaterRenderer& renderer() { return gpu; }
    const WaterRenderer& renderer() const { return gpu; }

    // --- Lifecycle (both halves) --------------------------------------
    void create(rhi::Device& device, ShaderLibrary& shaders,
                core::JobSystem& jobSystem);
    void destroy(rhi::Device& device);
    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders) {
        gpu.refreshPipeline(device, shaders);
    }

    // --- Baked water + pool map (renderer) ----------------------------
    void update(rhi::Device& device, const TerrainParams& params,
                const Vec3& cameraPos,
                sptr<const HeightField::Snapshot> field = nullptr) {
        gpu.update(device, params, cameraPos, std::move(field));
    }
    void setFarWater(FarWaterFn fn) { gpu.setFarWater(std::move(fn)); }
    void setBodies(sptr<const WaterBodies> next) {
        gpu.setBodies(std::move(next));
    }
    const WaterBodies* currentBodies() const { return gpu.currentBodies(); }
    Vec4 poolMapInfo() const { return gpu.poolMapInfo(); }
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup,
              rhi::BindGroupHandle sceneBindGroup) {
        gpu.draw(cmd, frameBindGroup, sceneBindGroup, sim);
    }

    // --- Real-time sim window (sim control) ---------------------------
    SimConfig& simConfig() { return sim.simConfig(); }
    const SimConfig& simConfig() const { return sim.simConfig(); }
    void setRainIntensity(f32 v) { sim.setRainIntensity(v); }
    void setSimSources(SimSourcesFn fn) { sim.setSimSources(std::move(fn)); }
    void setSimRuntimeSources(vector<terraingen::WaterSource> sources) {
        sim.setSimRuntimeSources(std::move(sources));
    }
    void notifySimGroundChanged() { sim.notifySimGroundChanged(); }
    bool simWindowIntersects(f32 minX, f32 minZ, f32 maxX,
                             f32 maxZ) const {
        return sim.simWindowIntersects(minX, minZ, maxX, maxZ);
    }
    // Once per frame, after update(). dt = real frame seconds: the sim
    // control drains, decides and kicks; the renderer then applies its
    // GPU requests (uploads, frozen meshes) in order.
    void updateSim(rhi::Device& device, const TerrainParams& params,
                   const Vec3& cameraPos, f32 dt);
    sptr<const terrain::WaterSimSnapshot> simSnapshot() const {
        return sim.simSnapshot();
    }
    Vec4 simMapInfo() const { return sim.simMapInfo(); }
    std::array<Vec4, 4> simFrozenInfo() const {
        return gpu.simFrozenInfo(sim.simConfig().debugMode);
    }
    Vec4 simTuneInfo() const { return sim.simTuneInfo(); }
    f32 simCostMs() const { return sim.simCostMs(); }
    bool simIsValid() const { return sim.simIsValid(); }
    bool simIsPreRolling() const { return sim.simIsPreRolling(); }
    bool simIsSettling() const { return sim.simIsSettling(); }
    u32 simWetCellCount() const { return sim.simWetCellCount(); }
    void requestSimDump(std::string path) {
        sim.requestSimDump(std::move(path));
    }

private:
    terrain::WaterSimControl sim;
    WaterRenderer gpu;
};

} // namespace render
