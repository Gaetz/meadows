#pragma once

#include "engine/platform/GlContext.hpp"
#include "engine/rhi/Device.hpp"

namespace rhi {

class GlCommandBuffer final : public CommandBuffer {
public:
    void beginRenderPass(const RenderPassDesc& desc) override;
    void endRenderPass() override;
};

// OpenGL 4.6 core backend (DSA, bindless). GL has no real command buffers or
// swapchain objects: "recording" executes immediately and present is a buffer
// swap. The Device interface hides both so callers stay backend-agnostic.
class GlDevice final : public Device {
public:
    static uptr<GlDevice> create(platform::Window& window);

    CommandBuffer& beginFrame() override;
    void endFrame() override;
    Backend backend() const override { return Backend::OpenGL; }

private:
    GlDevice(uptr<platform::GlContext> context, platform::Window& window);

    uptr<platform::GlContext> context;
    platform::Window& window;
    GlCommandBuffer commandBuffer;
};

} // namespace rhi
