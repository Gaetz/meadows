#pragma once

#include "engine/core/Defines.hpp"
#include "engine/rhi/Rhi.hpp"

namespace core {
class JobSystem;
}
namespace rhi {
class Device;
}
namespace render {
struct TerrainParams;
}

namespace game {

// Spectator/Edit minimap (dev ImGui window): the SAME map painter as the
// game map (MapRaster), baked on a worker around the camera at a
// selectable radius (1/5/10/100 km), camera centered. The 100 km ring
// works without baked tiles: off regions the raster reads the analytic
// mirror — distant country renders from pure math. Re-bakes when the
// camera strays past a fraction of the span, the radius changes, or —
// close rings only — new tiles publish (contentStamp).
class MiniMapPanel {
public:
    // Call each frame the window should exist (mode gate is the
    // caller's). Publishes a finished bake (main-thread texture upload,
    // Phase-5 idiom), kicks the next when needed, draws the window.
    void draw(rhi::Device& device, core::JobSystem& jobs,
              const render::TerrainParams& terrain, const Vec3& cameraPos,
              u64 contentStamp, bool* open);

    // Frees the GPU texture (scene teardown). A job still in flight
    // keeps its self-owned packet alive and is dropped unread.
    void shutdown(rhi::Device& device);

private:
    struct Job;
    sptr<Job> pending;
    rhi::TextureHandle texture {};
    bool hasTexture { false };
    f32 bakedCenterX { 0.0f };
    f32 bakedCenterZ { 0.0f };
    f32 bakedSpan { 0.0f };
    u32 radiusIndex { 1 }; // default 5 km
    u32 bakedRadius { ~0u };
    u64 bakedStamp { 0 };
};

} // namespace game
