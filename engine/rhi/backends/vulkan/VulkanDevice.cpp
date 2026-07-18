#include "engine/rhi/backends/vulkan/VulkanDevice.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include <vulkan/vulkan.h>

#include "engine/core/Log.hpp"
#include "engine/platform/VulkanSurface.hpp"
#include "engine/platform/Window.hpp"

namespace rhi {

namespace {

// Frames recorded ahead of the GPU. Per-frame resources (command buffer,
// acquire semaphore, fence) are duplicated this many times so the CPU never
// waits on the frame it is recording. The Phase-5 snapshot seam already passes
// render data by value, so deeper pipelining stays a scheduling choice.
constexpr u32 kFramesInFlight = 2;

bool vkOk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        LOG_ERROR("Vulkan: {} failed (VkResult {})", what,
                  static_cast<i32>(result));
        return false;
    }
    return true;
}

bool hasExtension(const vector<VkExtensionProperties>& available,
                  const char* name) {
    return std::any_of(available.begin(), available.end(),
                       [name](const VkExtensionProperties& e) {
                           return std::strcmp(e.extensionName, name) == 0;
                       });
}

vector<VkExtensionProperties> instanceExtensionProperties() {
    u32 count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    vector<VkExtensionProperties> props(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());
    return props;
}

vector<VkExtensionProperties> deviceExtensionProperties(VkPhysicalDevice gpu) {
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
    vector<VkExtensionProperties> props(count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, props.data());
    return props;
}

bool validationLayerAvailable() {
    u32 count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.end(),
                       [](const VkLayerProperties& l) {
                           return std::strcmp(l.layerName,
                                              "VK_LAYER_KHRONOS_validation") ==
                                  0;
                       });
}

// Records one frame. V1 only implements the render pass (so the backbuffer
// clears); every other entry point lands with V4/V5, which is why they are
// still no-ops rather than asserts — callers must not be able to tell whether
// a backend records or executes immediately (CommandBuffer contract).
class VulkanCommandBuffer final : public CommandBuffer {
public:
    void begin(VkCommandBuffer cb, VkRenderPass pass, VkFramebuffer fb,
               VkExtent2D extent) {
        cb_ = cb;
        pass_ = pass;
        framebuffer_ = fb;
        extent_ = extent;
        inPass_ = false;
    }

    void beginRenderPass(const RenderPassDesc& desc) override {
        if (cb_ == VK_NULL_HANDLE || inPass_) {
            return;
        }
        // V1 targets the swapchain only (desc.framebuffer is ignored until V4
        // creates real VkFramebuffers for offscreen targets).
        VkClearValue clear {};
        clear.color = { { desc.clearColor.r, desc.clearColor.g,
                          desc.clearColor.b, desc.clearColor.a } };

        VkRenderPassBeginInfo info {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = pass_;
        info.framebuffer = framebuffer_;
        info.renderArea.offset = { 0, 0 };
        info.renderArea.extent = extent_;
        info.clearValueCount = 1;
        info.pClearValues = &clear;
        vkCmdBeginRenderPass(cb_, &info, VK_SUBPASS_CONTENTS_INLINE);
        inPass_ = true;
    }

    void endRenderPass() override {
        if (cb_ != VK_NULL_HANDLE && inPass_) {
            vkCmdEndRenderPass(cb_);
            inPass_ = false;
        }
    }

    bool inPass() const { return inPass_; }

    // --- Not implemented before V4/V5 ---------------------------------------
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

private:
    VkCommandBuffer cb_ { VK_NULL_HANDLE };
    VkRenderPass pass_ { VK_NULL_HANDLE };
    VkFramebuffer framebuffer_ { VK_NULL_HANDLE };
    VkExtent2D extent_ {};
    bool inPass_ { false };
};

} // namespace

struct VulkanDevice::Impl {
    platform::Window* window { nullptr };

    VkInstance instance { VK_NULL_HANDLE };
    VkSurfaceKHR surface { VK_NULL_HANDLE };
    VkPhysicalDevice gpu { VK_NULL_HANDLE };
    VkDevice device { VK_NULL_HANDLE };

    u32 graphicsFamily { 0 };
    u32 presentFamily { 0 };
    VkQueue graphicsQueue { VK_NULL_HANDLE };
    VkQueue presentQueue { VK_NULL_HANDLE };

    // Swapchain + everything sized by it (recreated together on resize).
    VkSwapchainKHR swapchain { VK_NULL_HANDLE };
    VkFormat colorFormat { VK_FORMAT_UNDEFINED };
    VkExtent2D extent {};
    vector<VkImage> images;
    vector<VkImageView> imageViews;
    vector<VkFramebuffer> framebuffers;
    // Signalled when the frame targeting this IMAGE is done; present waits on
    // it. Per-image (not per-frame) so a semaphore is never reused while a
    // previous present is still pending on it.
    vector<VkSemaphore> renderFinished;

    VkRenderPass renderPass { VK_NULL_HANDLE };
    VkCommandPool commandPool { VK_NULL_HANDLE };

    // Per frame-in-flight.
    std::array<VkCommandBuffer, kFramesInFlight> commandBuffers {};
    std::array<VkSemaphore, kFramesInFlight> imageAvailable {};
    std::array<VkFence, kFramesInFlight> inFlight {};

    u32 frame { 0 };       // frame-in-flight slot being recorded
    u32 imageIndex { 0 };  // swapchain image acquired this frame
    bool frameActive { false }; // false when acquire failed -> endFrame skips

    VulkanCommandBuffer cmd;

    bool createSwapchain();
    void destroySwapchain();
    bool recreateSwapchain();
};

// --- Swapchain ---------------------------------------------------------------

bool VulkanDevice::Impl::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps {};
    if (!vkOk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps),
              "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
        return false;
    }

    // A zero extent means the window is minimized: nothing to create yet.
    extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = std::clamp(static_cast<u32>(window->width()),
                                  caps.minImageExtent.width,
                                  caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<u32>(window->height()),
                                   caps.minImageExtent.height,
                                   caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        return false;
    }

    u32 formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, nullptr);
    vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount,
                                         formats.data());
    if (formats.empty()) {
        LOG_ERROR("Vulkan: surface reports no formats");
        return false;
    }
    // Prefer a straight 8-bit BGRA UNORM target. The engine's color pipeline
    // manages its own gamma, so an _SRGB swapchain would double-correct.
    VkSurfaceFormatKHR chosen = formats[0];
    for (const VkSurfaceFormatKHR& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    colorFormat = chosen.format;

    u32 imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info {};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface;
    info.minImageCount = imageCount;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const std::array<u32, 2> families { graphicsFamily, presentFamily };
    if (graphicsFamily != presentFamily) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = families.data();
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // FIFO is the only mode guaranteed present everywhere (and is v-synced).
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = VK_NULL_HANDLE;

    if (!vkOk(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain),
              "vkCreateSwapchainKHR")) {
        return false;
    }

    u32 count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    images.resize(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());

    imageViews.resize(count);
    for (u32 i = 0; i < count; ++i) {
        VkImageViewCreateInfo view {};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = colorFormat;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (!vkOk(vkCreateImageView(device, &view, nullptr, &imageViews[i]),
                  "vkCreateImageView")) {
            return false;
        }
    }

    // Render pass: one color attachment, cleared on load, left ready to
    // present. V4 generalizes this to RenderPassDesc's load ops and to
    // offscreen targets with depth.
    VkAttachmentDescription color {};
    color.format = colorFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef {};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    // Keep the pass from starting to write before the image is acquired.
    VkSubpassDependency dep {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo pass {};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass.attachmentCount = 1;
    pass.pAttachments = &color;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = 1;
    pass.pDependencies = &dep;
    if (!vkOk(vkCreateRenderPass(device, &pass, nullptr, &renderPass),
              "vkCreateRenderPass")) {
        return false;
    }

    framebuffers.resize(count);
    renderFinished.resize(count);
    for (u32 i = 0; i < count; ++i) {
        VkFramebufferCreateInfo fb {};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = renderPass;
        fb.attachmentCount = 1;
        fb.pAttachments = &imageViews[i];
        fb.width = extent.width;
        fb.height = extent.height;
        fb.layers = 1;
        if (!vkOk(vkCreateFramebuffer(device, &fb, nullptr, &framebuffers[i]),
                  "vkCreateFramebuffer")) {
            return false;
        }
        VkSemaphoreCreateInfo sem {};
        sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (!vkOk(vkCreateSemaphore(device, &sem, nullptr, &renderFinished[i]),
                  "vkCreateSemaphore(renderFinished)")) {
            return false;
        }
    }
    return true;
}

void VulkanDevice::Impl::destroySwapchain() {
    if (device == VK_NULL_HANDLE) {
        return;
    }
    for (VkSemaphore s : renderFinished) {
        vkDestroySemaphore(device, s, nullptr);
    }
    renderFinished.clear();
    for (VkFramebuffer fb : framebuffers) {
        vkDestroyFramebuffer(device, fb, nullptr);
    }
    framebuffers.clear();
    if (renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, renderPass, nullptr);
        renderPass = VK_NULL_HANDLE;
    }
    for (VkImageView v : imageViews) {
        vkDestroyImageView(device, v, nullptr);
    }
    imageViews.clear();
    images.clear();
    if (swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }
}

bool VulkanDevice::Impl::recreateSwapchain() {
    vkDeviceWaitIdle(device);
    destroySwapchain();
    return createSwapchain();
}

// --- Lifetime ----------------------------------------------------------------

VulkanDevice::VulkanDevice() : impl { std::make_unique<Impl>() } {}

VulkanDevice::~VulkanDevice() {
    if (impl->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(impl->device);
        for (u32 i = 0; i < kFramesInFlight; ++i) {
            vkDestroySemaphore(impl->device, impl->imageAvailable[i], nullptr);
            vkDestroyFence(impl->device, impl->inFlight[i], nullptr);
        }
        impl->destroySwapchain();
        if (impl->commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(impl->device, impl->commandPool, nullptr);
        }
        vkDestroyDevice(impl->device, nullptr);
    }
    if (impl->instance != VK_NULL_HANDLE) {
        if (impl->surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(impl->instance, impl->surface, nullptr);
        }
        vkDestroyInstance(impl->instance, nullptr);
    }
}

// --- Frame -------------------------------------------------------------------

CommandBuffer& VulkanDevice::beginFrame() {
    Impl& d = *impl;
    d.frameActive = false;

    vkWaitForFences(d.device, 1, &d.inFlight[d.frame], VK_TRUE, UINT64_MAX);

    VkResult acquired =
        vkAcquireNextImageKHR(d.device, d.swapchain, UINT64_MAX,
                              d.imageAvailable[d.frame], VK_NULL_HANDLE,
                              &d.imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        // Window resized/minimized: rebuild and sit this frame out. The
        // acquire semaphore was not signalled, so nothing must be submitted.
        d.recreateSwapchain();
        return d.cmd;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        vkOk(acquired, "vkAcquireNextImageKHR");
        return d.cmd;
    }

    vkResetFences(d.device, 1, &d.inFlight[d.frame]);
    vkResetCommandBuffer(d.commandBuffers[d.frame], 0);

    VkCommandBufferBeginInfo begin {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!vkOk(vkBeginCommandBuffer(d.commandBuffers[d.frame], &begin),
              "vkBeginCommandBuffer")) {
        return d.cmd;
    }

    d.cmd.begin(d.commandBuffers[d.frame], d.renderPass,
                d.framebuffers[d.imageIndex], d.extent);
    d.frameActive = true;
    return d.cmd;
}

void VulkanDevice::endFrame() {
    Impl& d = *impl;
    if (!d.frameActive) {
        return; // acquire failed this frame — nothing was recorded
    }
    // A caller that forgot endRenderPass would leave the pass open; closing it
    // here keeps the command buffer valid rather than failing the submit.
    d.cmd.endRenderPass();

    VkCommandBuffer cb = d.commandBuffers[d.frame];
    if (!vkOk(vkEndCommandBuffer(cb), "vkEndCommandBuffer")) {
        d.frameActive = false;
        return;
    }

    const VkPipelineStageFlags waitStage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &d.imageAvailable[d.frame];
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &d.renderFinished[d.imageIndex];
    if (!vkOk(vkQueueSubmit(d.graphicsQueue, 1, &submit, d.inFlight[d.frame]),
              "vkQueueSubmit")) {
        d.frameActive = false;
        return;
    }

    VkPresentInfoKHR present {};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &d.renderFinished[d.imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &d.swapchain;
    present.pImageIndices = &d.imageIndex;
    const VkResult presented = vkQueuePresentKHR(d.presentQueue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR ||
        presented == VK_SUBOPTIMAL_KHR) {
        d.recreateSwapchain();
    } else {
        vkOk(presented, "vkQueuePresentKHR");
    }

    d.frame = (d.frame + 1) % kFramesInFlight;
    d.frameActive = false;
}

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

uptr<VulkanDevice> VulkanDevice::create(platform::Window& window) {
    auto self = uptr<VulkanDevice> { new VulkanDevice() };
    Impl& d = *self->impl;
    d.window = &window;

    // --- Instance ---
    vector<const char*> extensions = platform::vulkanInstanceExtensions();
    if (extensions.empty()) {
        return nullptr;
    }
    const vector<VkExtensionProperties> availableExt =
        instanceExtensionProperties();

    VkInstanceCreateFlags instanceFlags = 0;
    // MoltenVK is a *portability* driver: it is only enumerated when the
    // instance opts in explicitly.
    if (hasExtension(availableExt, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        instanceFlags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    vector<const char*> layers;
#ifndef NDEBUG
    if (validationLayerAvailable()) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        LOG_DEBUG("Vulkan: validation layer enabled");
    }
#endif

    VkApplicationInfo app {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Meadows";
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instanceInfo {};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.flags = instanceFlags;
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = static_cast<u32>(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    instanceInfo.enabledLayerCount = static_cast<u32>(layers.size());
    instanceInfo.ppEnabledLayerNames = layers.data();
    if (!vkOk(vkCreateInstance(&instanceInfo, nullptr, &d.instance),
              "vkCreateInstance")) {
        return nullptr;
    }

    // --- Surface ---
    d.surface = reinterpret_cast<VkSurfaceKHR>(
        platform::createVulkanSurface(window, d.instance));
    if (d.surface == VK_NULL_HANDLE) {
        return nullptr;
    }

    // --- Physical device ---
    u32 gpuCount = 0;
    vkEnumeratePhysicalDevices(d.instance, &gpuCount, nullptr);
    if (gpuCount == 0) {
        LOG_ERROR("Vulkan: no physical device found");
        return nullptr;
    }
    vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(d.instance, &gpuCount, gpus.data());

    i32 bestScore = -1;
    for (VkPhysicalDevice candidate : gpus) {
        const vector<VkExtensionProperties> devExt =
            deviceExtensionProperties(candidate);
        if (!hasExtension(devExt, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            continue;
        }
        u32 familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount,
                                                 nullptr);
        vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount,
                                                 families.data());
        bool haveGraphics = false;
        bool havePresent = false;
        u32 graphics = 0;
        u32 present = 0;
        for (u32 i = 0; i < familyCount; ++i) {
            if (!haveGraphics &&
                (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                graphics = i;
                haveGraphics = true;
            }
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, d.surface,
                                                 &supported);
            if (!havePresent && supported) {
                present = i;
                havePresent = true;
            }
        }
        if (!haveGraphics || !havePresent) {
            continue;
        }
        VkPhysicalDeviceProperties props {};
        vkGetPhysicalDeviceProperties(candidate, &props);
        // Prefer a discrete GPU; on the M1 there is only the integrated one.
        const i32 score =
            props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
        if (score > bestScore) {
            bestScore = score;
            d.gpu = candidate;
            d.graphicsFamily = graphics;
            d.presentFamily = present;
        }
    }
    if (d.gpu == VK_NULL_HANDLE) {
        LOG_ERROR("Vulkan: no device with graphics + present + swapchain");
        return nullptr;
    }

    VkPhysicalDeviceProperties props {};
    vkGetPhysicalDeviceProperties(d.gpu, &props);

    // --- Logical device ---
    vector<const char*> deviceExtensions { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    const vector<VkExtensionProperties> devExt = deviceExtensionProperties(d.gpu);
    // Mandatory when present (MoltenVK): the spec requires enabling it on a
    // portability driver.
    if (hasExtension(devExt, "VK_KHR_portability_subset")) {
        deviceExtensions.push_back("VK_KHR_portability_subset");
    }

    const f32 priority = 1.0f;
    vector<VkDeviceQueueCreateInfo> queueInfos;
    queueInfos.push_back({ .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                           .pNext = nullptr,
                           .flags = 0,
                           .queueFamilyIndex = d.graphicsFamily,
                           .queueCount = 1,
                           .pQueuePriorities = &priority });
    if (d.presentFamily != d.graphicsFamily) {
        queueInfos.push_back(
            { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
              .pNext = nullptr,
              .flags = 0,
              .queueFamilyIndex = d.presentFamily,
              .queueCount = 1,
              .pQueuePriorities = &priority });
    }

    VkDeviceCreateInfo deviceInfo {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = static_cast<u32>(queueInfos.size());
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.enabledExtensionCount =
        static_cast<u32>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    if (!vkOk(vkCreateDevice(d.gpu, &deviceInfo, nullptr, &d.device),
              "vkCreateDevice")) {
        return nullptr;
    }
    vkGetDeviceQueue(d.device, d.graphicsFamily, 0, &d.graphicsQueue);
    vkGetDeviceQueue(d.device, d.presentFamily, 0, &d.presentQueue);

    // --- Swapchain + per-frame objects ---
    if (!d.createSwapchain()) {
        return nullptr;
    }

    VkCommandPoolCreateInfo poolInfo {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = d.graphicsFamily;
    if (!vkOk(vkCreateCommandPool(d.device, &poolInfo, nullptr, &d.commandPool),
              "vkCreateCommandPool")) {
        return nullptr;
    }

    VkCommandBufferAllocateInfo alloc {};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = d.commandPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = kFramesInFlight;
    if (!vkOk(vkAllocateCommandBuffers(d.device, &alloc,
                                       d.commandBuffers.data()),
              "vkAllocateCommandBuffers")) {
        return nullptr;
    }

    for (u32 i = 0; i < kFramesInFlight; ++i) {
        VkSemaphoreCreateInfo sem {};
        sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (!vkOk(vkCreateSemaphore(d.device, &sem, nullptr,
                                    &d.imageAvailable[i]),
                  "vkCreateSemaphore(imageAvailable)")) {
            return nullptr;
        }
        // Created signalled so the first beginFrame does not block forever.
        VkFenceCreateInfo fence {};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!vkOk(vkCreateFence(d.device, &fence, nullptr, &d.inFlight[i]),
                  "vkCreateFence")) {
            return nullptr;
        }
    }

    // Caps stay all-false until the features behind them actually exist
    // (V2 resources, V3 shaders, V4 pipelines): renderer systems gate on these
    // flags, so advertising early would make them call into no-ops.
    LOG_INFO("Vulkan device ready: {} — {}x{}, {} swapchain images "
             "(V1: clear only)",
             props.deviceName, d.extent.width, d.extent.height,
             static_cast<u32>(d.images.size()));
    return self;
}

uptr<Device> createVulkanDevice(platform::Window& window) {
    return VulkanDevice::create(window);
}

} // namespace rhi
