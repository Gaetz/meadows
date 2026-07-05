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

    // Scissor rectangle (Vulkan dynamic scissor), origin bottom-left like
    // the viewport. beginRenderPass resets it to disabled/full-target.
    virtual void setScissor(u32 x, u32 y, u32 width, u32 height) = 0;
    virtual void clearScissor() = 0;

    // Winding of front faces for subsequent draws. beginRenderPass resets it
    // to CounterClockwise; mirrored passes set Clockwise once.
    virtual void setFrontFace(FrontFace frontFace) = 0;

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

    // Copies the full base level of `src` into `dst` (same size and format;
    // Vulkan vkCmdCopyImage). Call OUTSIDE a render pass — the intended use
    // is snapshotting scene color/depth between passes so a later pass can
    // sample them (sampling a bound attachment is undefined).
    virtual void copyTexture(TextureHandle src, TextureHandle dst) = 0;

    // GPU-side buffer copy (Vulkan vkCmdCopyBuffer). Pairs a device-local
    // buffer with a `readback` staging buffer: the GPU result is copied
    // into the staging copy, which the CPU reads without dragging the
    // working buffer out of VRAM.
    virtual void copyBuffer(BufferHandle src, BufferHandle dst, u64 size,
                            u64 srcOffset = 0, u64 dstOffset = 0) = 0;

    // Compute (caps.computeShaders). setPipeline accepts compute pipelines
    // too (only the program binds — no raster state is touched). Call
    // outside render passes.
    virtual void dispatch(u32 groupsX, u32 groupsY = 1, u32 groupsZ = 1) = 0;
    // Makes compute writes (SSBOs, sampled textures) visible to subsequent
    // GPU work and CPU readback (Vulkan vkCmdPipelineBarrier).
    virtual void memoryBarrier() = 0;
};

} // namespace rhi
