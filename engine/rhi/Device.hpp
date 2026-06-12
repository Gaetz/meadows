#pragma once

#include "engine/core/Defines.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Rhi.hpp"

namespace platform {
class Window;
}

namespace rhi {

// Owns the GPU: context/swapchain, resources, frame submission. One per
// application. Everything that talks to the graphics API goes through here
// or through the CommandBuffer it hands out (§2.1).
class Device {
public:
    virtual ~Device() = default;

    // Runtime backend selection (§2.1). Returns nullptr (with a logged error)
    // when the backend cannot be initialized.
    static uptr<Device> create(Backend backend, platform::Window& window);

    // Begins the frame and returns the command buffer to record into.
    // The reference is valid until endFrame().
    virtual CommandBuffer& beginFrame() = 0;

    // Submits the recorded work and presents the backbuffer.
    virtual void endFrame() = 0;

    virtual Backend backend() const = 0;

    // Resource creation (createBuffer / createTexture / createPipeline /
    // createBindGroup) arrives with the sprite renderer milestone.
};

} // namespace rhi
