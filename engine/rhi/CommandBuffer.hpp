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

    // setPipeline / setBindGroup / setVertexBuffer / draw arrive with the
    // sprite renderer milestone.
};

} // namespace rhi
