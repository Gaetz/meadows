#pragma once

#include "engine/core/Defines.hpp"
#include "engine/rhi/Rhi.hpp"

namespace rhi {
class CommandBuffer;
class Device;
}

namespace render {

class ShaderLibrary;

// Clustered-forward light culling (docs/RENDERING.md §5): one compute pass
// assigns the LightsUbo's lights to the cells of a frustum grid that
// shares the froxel fog's exponential z slicing (shaders/clusters.glsl —
// a froxel maps to its cluster by xy downsample, identical z). Surface
// shaders and the froxel inject then loop only their cell's list.
//
// The cluster SSBO rides binding 4 of the renderer's FRAME bind group, so
// every pass that binds it sees the lists; createBuffer() must therefore
// run before that group is built. Grid extents live in clusters.glsl and
// must match the constants here.
class LightClusters {
public:
    static constexpr u32 kClusterX = 16, kClusterY = 9, kClusterZ = 64;
    static constexpr u32 kSlots = 32; // slot 0 = count, 1..31 = indices

    void createBuffer(rhi::Device& device);
    void createPipeline(rhi::Device& device, ShaderLibrary& shaders);
    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);
    void destroy(rhi::Device& device);

    // Records the culling dispatch + the barrier that makes the lists
    // visible to the passes that follow. The caller has already bound the
    // frame bind group (FrameUbo + LightsUbo + the cluster SSBO).
    void run(rhi::CommandBuffer& cmd);

    bool ready() const { return pipeline.id != 0 && buffer.id != 0; }
    rhi::BufferHandle clusterBuffer() const { return buffer; }

private:
    rhi::BufferHandle buffer {};
    rhi::PipelineHandle pipeline {};
    u64 generation { 0 };
};

} // namespace render
