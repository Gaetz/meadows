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

    // Dev-tool escape hatch (chantier 8, anim preview): the backend-native
    // name behind a TextureHandle, so offscreen targets can be handed to the
    // ImGui backend (imgui_impl_opengl3 wants the raw GLuint as ImTextureID).
    // 0 = unknown handle or a backend without a meaningful native id. NOT a
    // rendering path — engine draws keep going through bind groups.
    virtual u64 nativeTextureId(TextureHandle /*handle*/) const { return 0; }

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

    // Synchronous GPU->CPU readback (Vulkan: staging copy + fence). NOTE
    // (perf P1): "written last frame" is NOT enough to avoid a pipeline
    // stall — the driver runs 1-2 frames deep. Gate the read behind a
    // fence (below) and keep the previous result while it is pending.
    virtual void readBuffer(BufferHandle handle, void* dst, u64 size,
                            u64 offset = 0) = 0;

    // GPU progress marker (GL sync object / Vulkan fence): insertFence
    // drops a marker after everything submitted so far; fenceReady polls
    // WITHOUT blocking and releases the fence once signaled (a handle is
    // single-use). destroyFence abandons a pending one (teardown).
    virtual FenceHandle insertFence() = 0;
    virtual bool fenceReady(FenceHandle handle) = 0;
    virtual void destroyFence(FenceHandle handle) = 0;

    // GPU clock marker (GL timer query / Vulkan timestamp — GPU-PERF P0):
    // insertTimestamp records the GPU clock when the stream REACHES this
    // point; timestampReady polls WITHOUT blocking and, once available,
    // writes the time (nanoseconds) and releases the handle (single-use,
    // like fences). destroyTimestamp abandons a pending one (teardown).
    // Gate on caps().timerQueries.
    virtual TimestampHandle insertTimestamp() = 0;
    virtual bool timestampReady(TimestampHandle handle, u64& nanos) = 0;
    virtual void destroyTimestamp(TimestampHandle handle) = 0;

    virtual BindGroupHandle createBindGroup(const BindGroupDesc& desc) = 0;
    virtual void destroyBindGroup(BindGroupHandle handle) = 0;
};

} // namespace rhi
