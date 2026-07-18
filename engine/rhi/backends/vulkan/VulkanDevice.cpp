#include "engine/rhi/backends/vulkan/VulkanDevice.hpp"

#include <vulkan/vulkan.h>

#include "engine/core/Log.hpp"
#include "engine/platform/Window.hpp"

namespace rhi {

namespace {

// Stub command buffer: the recording surface handed out by beginFrame. Every
// method is a no-op for now; V5 fills it with real vkCmd* recording. It exists
// already so the Device interface is satisfiable and callers compile unchanged.
class VulkanCommandBuffer final : public CommandBuffer {
public:
    void beginRenderPass(const RenderPassDesc&) override {}
    void endRenderPass() override {}
    void setViewport(u32, u32, u32, u32) override {}
    void setScissor(u32, u32, u32, u32) override {}
    void clearScissor() override {}
    void setFrontFace(FrontFace) override {}
    void setPipeline(PipelineHandle) override {}
    void setBindGroup(u32, BindGroupHandle) override {}
    void setVertexBuffer(u32, BufferHandle) override {}
    void setIndexBuffer(BufferHandle, IndexFormat) override {}
    void draw(u32, u32, u32) override {}
    void drawIndexed(u32, u32, u32, u32) override {}
    void copyTexture(TextureHandle, TextureHandle) override {}
    void copyBuffer(BufferHandle, BufferHandle, u64, u64, u64) override {}
    void dispatch(u32, u32, u32) override {}
    void memoryBarrier() override {}
};

} // namespace

struct VulkanDevice::Impl {
    // V1 populates this with VkInstance, VkPhysicalDevice, VkDevice, queues,
    // swapchain, per-frame sync, command pools, VMA allocator, etc.
    VulkanCommandBuffer commandBuffer;
};

VulkanDevice::VulkanDevice() : impl { std::make_unique<Impl>() } {}
VulkanDevice::~VulkanDevice() = default;

// --- Frame -------------------------------------------------------------------

CommandBuffer& VulkanDevice::beginFrame() { return impl->commandBuffer; }
void VulkanDevice::endFrame() {}

u64 VulkanDevice::nativeTextureId(TextureHandle) const { return 0; }

// --- Resources (stubbed until V2/V3/V4) --------------------------------------

BufferHandle VulkanDevice::createBuffer(const BufferDesc&, const void*) {
    return {};
}
void VulkanDevice::updateBuffer(BufferHandle, const void*, u64, u64) {}
void VulkanDevice::destroyBuffer(BufferHandle) {}

TextureHandle VulkanDevice::createTexture(const TextureDesc&, const void*) {
    return {};
}
void VulkanDevice::destroyTexture(TextureHandle) {}
void VulkanDevice::generateMipmaps(TextureHandle) {}

SamplerHandle VulkanDevice::createSampler(const SamplerDesc&) { return {}; }
void VulkanDevice::destroySampler(SamplerHandle) {}

FramebufferHandle VulkanDevice::createFramebuffer(const FramebufferDesc&) {
    return {};
}
void VulkanDevice::destroyFramebuffer(FramebufferHandle) {}

ShaderHandle VulkanDevice::createShader(const ShaderDesc&) { return {}; }
void VulkanDevice::destroyShader(ShaderHandle) {}

PipelineHandle VulkanDevice::createPipeline(const PipelineDesc&) { return {}; }
void VulkanDevice::destroyPipeline(PipelineHandle) {}
PipelineHandle VulkanDevice::createComputePipeline(const ComputePipelineDesc&) {
    return {};
}

void VulkanDevice::readBuffer(BufferHandle, void*, u64, u64) {}

FenceHandle VulkanDevice::insertFence() { return {}; }
bool VulkanDevice::fenceReady(FenceHandle) { return true; }
void VulkanDevice::destroyFence(FenceHandle) {}

TimestampHandle VulkanDevice::insertTimestamp() { return {}; }
bool VulkanDevice::timestampReady(TimestampHandle, u64&) { return false; }
void VulkanDevice::destroyTimestamp(TimestampHandle) {}

BindGroupHandle VulkanDevice::createBindGroup(const BindGroupDesc&) {
    return {};
}
void VulkanDevice::destroyBindGroup(BindGroupHandle) {}

// --- Creation ----------------------------------------------------------------

uptr<VulkanDevice> VulkanDevice::create(platform::Window& /*window*/) {
    // V0 scaffolding: prove the Vulkan loader (MoltenVK ICD on macOS) links and
    // is reachable, then decline so the fallback chain keeps using GL until V1
    // brings up a real instance + swapchain (docs/VULKAN.md).
    u32 apiVersion = 0;
    if (vkEnumerateInstanceVersion(&apiVersion) != VK_SUCCESS) {
        LOG_ERROR("Vulkan: vkEnumerateInstanceVersion failed (no loader?)");
        return nullptr;
    }
    LOG_INFO("Vulkan loader present: instance API {}.{}.{} — backend scaffolding "
             "only (V0), declining to GL fallback",
             VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion),
             VK_API_VERSION_PATCH(apiVersion));
    return nullptr;
}

uptr<Device> createVulkanDevice(platform::Window& window) {
    return VulkanDevice::create(window);
}

} // namespace rhi
