#pragma once

#include "engine/rhi/Rhi.hpp"

namespace rhi {

// Records the GPU work of one frame. The GL backend executes "recorded" calls
// immediately; a Vulkan backend will record into a real command buffer —
// callers cannot tell the difference and must not assume either behavior.
class CommandBuffer {
public:
    virtual ~CommandBuffer() = default;

    virtual void beginRenderPass(const RenderPassDesc& desc) = 0;
    virtual void endRenderPass() = 0;

    // Overrides the viewport set by beginRenderPass (full target size).
    virtual void setViewport(u32 x, u32 y, u32 width, u32 height) = 0;

    virtual void setPipeline(PipelineHandle pipeline) = 0;

    // `index` is the bind-group slot (future Vulkan descriptor-set index).
    // The GL backend ignores it: entries carry explicit binding points.
    virtual void setBindGroup(u32 index, BindGroupHandle group) = 0;

    // `slot` matches PipelineDesc::vertexBuffers. Call after setPipeline.
    virtual void setVertexBuffer(u32 slot, BufferHandle buffer) = 0;
    virtual void setIndexBuffer(BufferHandle buffer, IndexFormat format) = 0;

    virtual void draw(u32 vertexCount, u32 instanceCount = 1,
                      u32 firstVertex = 0) = 0;
    virtual void drawIndexed(u32 indexCount, u32 instanceCount = 1,
                             u32 firstIndex = 0, u32 firstInstance = 0) = 0;
};

} // namespace rhi
