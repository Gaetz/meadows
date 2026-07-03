#pragma once

#include "engine/core/Defines.hpp"
#include "engine/rhi/Rhi.hpp"

namespace rhi {
class CommandBuffer;
class Device;
}

namespace render {

class ShaderLibrary;

// Water surface (brick 18): one large quad at sea level following the camera
// (snapped to the chunk grid), shaded per pixel — procedural scrolling wave
// normals, fresnel between a sky reflection (planar reflections replace it
// in brick 19) and a REFRACTED scene color (sampled from the pre-water scene
// snapshot, distorted by the waves, absorbed with depth), plus depth-based
// shore foam. Renders into the HDR target after the opaque pass, depth-tested
// against it (terrain above sea level occludes normally).
class WaterSystem {
public:
    void create(rhi::Device& device, ShaderLibrary& shaders);
    void destroy(rhi::Device& device);
    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // `sceneBindGroup` holds the pre-water scene color+depth snapshot
    // (texture units 0 and 1) — owned by the scene, since the snapshot
    // textures track the window size.
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup,
              rhi::BindGroupHandle sceneBindGroup);

private:
    void buildPipeline(rhi::Device& device, ShaderLibrary& shaders);

    rhi::BufferHandle vertexBuffer {};
    rhi::BufferHandle indexBuffer {};
    rhi::PipelineHandle pipeline {};
    u64 shaderGeneration { 0 };
};

} // namespace render
