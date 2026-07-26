#pragma once

#include "engine/rhi/Device.hpp"

namespace platform {
class Window;
}

namespace rhi {

// Vulkan RHI backend (+ MoltenVK on macOS). Built only when MEADOWS_RHI_VULKAN
// is ON; selected at runtime by the Device::create fallback chain (§2.1).
//
// Vulkan is the intended FINAL renderer; the bring-up journal and design
// notes live in docs/VULKAN.md. All Vulkan types stay in
// the .cpp (pimpl) so this header — and every RHI header — stays free of any
// <vulkan/*> include (§3.1).
class VulkanDevice final : public Device {
public:
    // Returns nullptr (with a logged reason) if a Vulkan device usable for
    // presenting to `window` cannot be created — the caller then falls back to
    // the next backend in the preference chain.
    static uptr<VulkanDevice> create(platform::Window& window);

    ~VulkanDevice() override;

    // --- Frame ---------------------------------------------------------------
    CommandBuffer& beginFrame() override;
    void endFrame() override;
    CommandBuffer* asyncComputeCmd() override;
    void endAsyncCompute() override;

    Backend backend() const override { return Backend::Vulkan; }
    const DeviceCaps& caps() const override { return caps_; }
    u64 nativeTextureId(TextureHandle handle) const override;

    // --- Resources -----------------------------------------------------------
    BufferHandle createBuffer(const BufferDesc& desc,
                              const void* initialData) override;
    void updateBuffer(BufferHandle handle, const void* data, u64 size,
                      u64 offset) override;
    void destroyBuffer(BufferHandle handle) override;

    TextureHandle createTexture(const TextureDesc& desc,
                                const void* pixels) override;
    void destroyTexture(TextureHandle handle) override;
    void generateMipmaps(TextureHandle handle) override;

    SamplerHandle createSampler(const SamplerDesc& desc) override;
    void destroySampler(SamplerHandle handle) override;

    FramebufferHandle createFramebuffer(const FramebufferDesc& desc) override;
    void destroyFramebuffer(FramebufferHandle handle) override;

    ShaderHandle createShader(const ShaderDesc& desc) override;
    void destroyShader(ShaderHandle handle) override;

    PipelineHandle createPipeline(const PipelineDesc& desc) override;
    void destroyPipeline(PipelineHandle handle) override;
    PipelineHandle createComputePipeline(
        const ComputePipelineDesc& desc) override;

    void readBuffer(BufferHandle handle, void* dst, u64 size,
                    u64 offset) override;

    FenceHandle insertFence() override;
    bool fenceReady(FenceHandle handle) override;
    void destroyFence(FenceHandle handle) override;

    TimestampHandle insertTimestamp() override;
    bool timestampReady(TimestampHandle handle, u64& nanos) override;
    void destroyTimestamp(TimestampHandle handle) override;

    BindGroupHandle createBindGroup(const BindGroupDesc& desc) override;
    void destroyBindGroup(BindGroupHandle handle) override;

    // Every Vulkan handle (instance, device, swapchain, command pools…) lives
    // behind this pimpl so no <vulkan/*> type leaks into a header (§3.1). Only
    // the NAME is public — the type stays incomplete outside VulkanDevice.cpp,
    // where the backend's own CommandBuffer needs to name it to reach the
    // resource tables.
    struct Impl;

private:
    VulkanDevice();

    DeviceCaps caps_ {};

    uptr<Impl> impl;
};

// Factory entry point called by Device::create (Device.cpp), guarded by
// MEADOWS_RHI_VULKAN. Returns nullptr on failure (fallback to GL).
uptr<Device> createVulkanDevice(platform::Window& window);

} // namespace rhi
