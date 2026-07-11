#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"
#include "engine/rhi/Rhi.hpp"
#include "engine/rhi/UniqueHandle.hpp"

namespace rhi {
class Device;
}
namespace engine {
struct FrameContext;
}

namespace render {

class ShaderLibrary;

// Chantier P0 C1 — the 3D particle pass: camera-facing quads pulled from
// an instance SSBO (gl_VertexID corners, no vertex buffers — the rain
// pattern with CPU-simulated data). Two batches per frame: ALPHA drawn
// far-to-near (the caller pre-sorts), then ADDITIVE (order-free). Soft
// round falloff lives in the shader — no texture needed until an asset
// asks. Depth-tested against the opaques, never writing (transparents).
//
// Renderer-side only: the SIM stays fx::ParticleSim (headless); the
// scene's extract copies live particles into FxInstance PODs on the
// snapshot — this class never sees the sim or the world (Phase-5 seam).
struct FxInstance {
    Vec4 positionSize; // xyz = world center, w = quad size (m)
    Vec4 color;        // premultiplied nothing — straight RGBA
};

class FxRenderer {
public:
    void create(rhi::Device& device, ShaderLibrary& shaders);
    void destroy(rhi::Device& device);

    // Draw inside the main pass, after the opaques (the rain slot).
    // `alpha` must arrive far-to-near; `additive` in any order.
    void draw(engine::FrameContext& frame, ShaderLibrary& shaders,
              rhi::BindGroupHandle frameGroup,
              const vector<FxInstance>& alpha,
              const vector<FxInstance>& additive);

private:
    void ensurePipelines(rhi::Device& device, ShaderLibrary& shaders);
    // Uploads the batch at the SSBO's base and issues 6 verts/instance —
    // called once per blend batch (GL keeps the two draws coherent).
    void drawBatch(engine::FrameContext& frame,
                   const vector<FxInstance>& batch,
                   rhi::PipelineHandle pipeline,
                   rhi::BindGroupHandle frameGroup);

    rhi::UniquePipeline alphaPipeline;
    rhi::UniquePipeline additivePipeline;
    rhi::UniqueBuffer instances;
    rhi::UniqueBindGroup group;
    u32 capacity { 0 }; // FxInstance slots in the SSBO
    u64 shaderGeneration { 0 };
};

} // namespace render
