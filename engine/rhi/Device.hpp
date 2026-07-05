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

    // Per-feature capabilities (§ degraded-mode goal): renderer systems check
    // the flags they need instead of testing GL versions.
    virtual const DeviceCaps& caps() const = 0;

    // --- Resources -----------------------------------------------------------
    // All creation functions return a 0 handle (with a logged error) on
    // failure. Destroying a 0 handle is a no-op.

    virtual BufferHandle createBuffer(const BufferDesc& desc,
                                      const void* initialData = nullptr) = 0;
    // Queue-style write: the data is visible to the next frame's commands.
    // Honors BufferDesc::dynamic for the upload strategy.
    virtual void updateBuffer(BufferHandle handle, const void* data, u64 size,
                              u64 offset = 0) = 0;
    virtual void destroyBuffer(BufferHandle handle) = 0;

    // `pixels` is tightly packed, desc.width * desc.height * arrayLayers
    // texels (base mip only; see TextureDesc).
    virtual TextureHandle createTexture(const TextureDesc& desc,
                                        const void* pixels) = 0;
    virtual void destroyTexture(TextureHandle handle) = 0;
    // Fills mip levels 1..N from the base level (requires mipLevels > 1).
    virtual void generateMipmaps(TextureHandle handle) = 0;

    virtual SamplerHandle createSampler(const SamplerDesc& desc) = 0;
    virtual void destroySampler(SamplerHandle handle) = 0;

    virtual FramebufferHandle createFramebuffer(const FramebufferDesc& desc) = 0;
    virtual void destroyFramebuffer(FramebufferHandle handle) = 0;

    virtual ShaderHandle createShader(const ShaderDesc& desc) = 0;
    virtual void destroyShader(ShaderHandle handle) = 0;

    virtual PipelineHandle createPipeline(const PipelineDesc& desc) = 0;
    virtual void destroyPipeline(PipelineHandle handle) = 0;

    // Compute pipeline (caps.computeShaders): destroyed via destroyPipeline.
    virtual PipelineHandle createComputePipeline(
        const ComputePipelineDesc& desc) = 0;

    // Synchronous GPU->CPU readback (Vulkan: staging copy + fence). Intended
    // for small buffers written LAST frame (compute culling results): by
    // then the GPU is done and the stall is negligible.
    virtual void readBuffer(BufferHandle handle, void* dst, u64 size,
                            u64 offset = 0) = 0;

    virtual BindGroupHandle createBindGroup(const BindGroupDesc& desc) = 0;
    virtual void destroyBindGroup(BindGroupHandle handle) = 0;
};

} // namespace rhi
