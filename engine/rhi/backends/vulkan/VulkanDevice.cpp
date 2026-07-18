#include "engine/rhi/backends/vulkan/VulkanDevice.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>

#include <vulkan/vulkan.h>

// VMA is header-only; this is the single TU that emits its implementation.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

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

// --- Format / state conversion ----------------------------------------------

VkFormat toVkFormat(TextureFormat format) {
    switch (format) {
    case TextureFormat::RGBA8:    return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::SRGBA8:   return VK_FORMAT_R8G8B8A8_SRGB;
    case TextureFormat::RGBA16F:  return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TextureFormat::R16F:     return VK_FORMAT_R16_SFLOAT;
    case TextureFormat::R32F:     return VK_FORMAT_R32_SFLOAT;
    case TextureFormat::Depth32F: return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

bool isDepthFormat(TextureFormat format) {
    return format == TextureFormat::Depth32F;
}

// Bytes per texel, for staging uploads. Only the formats that accept initial
// pixels (RGBA8/SRGBA8) are ever uploaded, but the others are sized here too
// so readbacks/copies can reason about them.
u32 bytesPerTexel(TextureFormat format) {
    switch (format) {
    case TextureFormat::RGBA8:
    case TextureFormat::SRGBA8:   return 4;
    case TextureFormat::RGBA16F:  return 8;
    case TextureFormat::R16F:     return 2;
    case TextureFormat::R32F:
    case TextureFormat::Depth32F: return 4;
    }
    return 4;
}

VkFilter toVkFilter(FilterMode filter) {
    return filter == FilterMode::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerAddressMode toVkAddressMode(AddressMode mode) {
    return mode == AddressMode::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                       : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

VkCompareOp toVkCompareOp(CompareFunc compare) {
    switch (compare) {
    case CompareFunc::Never:        return VK_COMPARE_OP_NEVER;
    case CompareFunc::Less:         return VK_COMPARE_OP_LESS;
    case CompareFunc::Equal:        return VK_COMPARE_OP_EQUAL;
    case CompareFunc::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareFunc::Greater:      return VK_COMPARE_OP_GREATER;
    case CompareFunc::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
    case CompareFunc::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareFunc::Always:       return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_NEVER;
}

VkBufferUsageFlags toVkBufferUsage(BufferUsage usage) {
    switch (usage) {
    case BufferUsage::Vertex:  return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    case BufferUsage::Index:   return VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    case BufferUsage::Uniform: return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    case BufferUsage::Storage: return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
}

// Access/stage masks for the layout transitions this backend performs. Kept
// deliberately narrow: only the transitions the upload/mipmap/copy paths use.
void layoutMasks(VkImageLayout layout, VkAccessFlags& access,
                 VkPipelineStageFlags& stage) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
        access = 0;
        stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        access = VK_ACCESS_TRANSFER_WRITE_BIT;
        stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        access = VK_ACCESS_TRANSFER_READ_BIT;
        stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        access = VK_ACCESS_SHADER_READ_BIT;
        stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        break;
    case VK_IMAGE_LAYOUT_GENERAL:
        access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        break;
    default:
        access = 0;
        stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        break;
    }
}

void transitionLayout(VkCommandBuffer cb, VkImage image,
                      VkImageAspectFlags aspect, u32 baseMip, u32 mipCount,
                      u32 layerCount, VkImageLayout oldLayout,
                      VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = baseMip;
    barrier.subresourceRange.levelCount = mipCount;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = layerCount;

    VkPipelineStageFlags srcStage = 0;
    VkPipelineStageFlags dstStage = 0;
    layoutMasks(oldLayout, barrier.srcAccessMask, srcStage);
    layoutMasks(newLayout, barrier.dstAccessMask, dstStage);

    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

// --- Resource records --------------------------------------------------------

struct VulkanBuffer {
    VkBuffer buffer { VK_NULL_HANDLE };
    VmaAllocation allocation { nullptr };
    u64 size { 0 };
    // Persistently mapped when the buffer lives in host-visible memory
    // (BufferDesc::dynamic or ::readback); nullptr means device-local, which
    // is written/read through a staging copy instead.
    void* mapped { nullptr };
};

struct VulkanTexture {
    VkImage image { VK_NULL_HANDLE };
    VmaAllocation allocation { nullptr };
    VkImageView view { VK_NULL_HANDLE };
    VkFormat format { VK_FORMAT_UNDEFINED };
    VkExtent3D extent {};
    u32 mipLevels { 1 };
    u32 arrayLayers { 1 };
    VkImageAspectFlags aspect { VK_IMAGE_ASPECT_COLOR_BIT };
    // Tracked so transitions know where they are coming from. One layout for
    // the whole image: this backend never leaves mips in mixed layouts outside
    // of generateMipmaps, which restores a uniform one before returning.
    VkImageLayout layout { VK_IMAGE_LAYOUT_UNDEFINED };
};

class VulkanCommandBuffer;

} // namespace

// --- Device state -------------------------------------------------------------

struct VulkanDevice::Impl {
    platform::Window* window { nullptr };

    VkInstance instance { VK_NULL_HANDLE };
    VkSurfaceKHR surface { VK_NULL_HANDLE };
    VkPhysicalDevice gpu { VK_NULL_HANDLE };
    VkDevice device { VK_NULL_HANDLE };
    VmaAllocator allocator { nullptr };

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
    // Separate transient pool for the blocking upload/readback submits, so
    // staging work never resets a frame's command buffer.
    VkCommandPool transferPool { VK_NULL_HANDLE };

    std::array<VkCommandBuffer, kFramesInFlight> commandBuffers {};
    std::array<VkSemaphore, kFramesInFlight> imageAvailable {};
    std::array<VkFence, kFramesInFlight> inFlight {};

    u32 frame { 0 };            // frame-in-flight slot being recorded
    u32 imageIndex { 0 };       // swapchain image acquired this frame
    bool frameActive { false }; // false when acquire failed -> endFrame skips

    // Handle tables. Ids start at 1 so 0 stays the invalid handle (§ Rhi.hpp).
    u32 nextId { 1 };
    std::unordered_map<u32, VulkanBuffer> buffers;
    std::unordered_map<u32, VulkanTexture> textures;
    std::unordered_map<u32, VkSampler> samplers;

    uptr<VulkanCommandBuffer> cmd;

    bool createSwapchain();
    void destroySwapchain();
    bool recreateSwapchain();

    // Records `record` into a one-shot command buffer and blocks until the GPU
    // is done. Uploads and readbacks are rare and setup-time, so a simple
    // blocking submit is the right trade; the async transfer queue (a reserved
    // lever, docs/VULKAN.md) can replace it later without touching callers.
    template <typename F>
    bool immediateSubmit(F&& record);

    VulkanBuffer* findBuffer(BufferHandle handle) {
        auto it = buffers.find(handle.id);
        return it == buffers.end() ? nullptr : &it->second;
    }
    VulkanTexture* findTexture(TextureHandle handle) {
        auto it = textures.find(handle.id);
        return it == textures.end() ? nullptr : &it->second;
    }

    // Allocates a host-visible scratch buffer for one upload/readback.
    bool createStaging(u64 size, bool forRead, VkBuffer& out,
                       VmaAllocation& outAlloc, void** outMapped);
};

namespace {

// Records one frame. V1/V2 implement the render pass and the copy commands;
// draws, binds and dispatch land with V4/V5, which is why they are still
// no-ops rather than asserts — callers must not be able to tell whether a
// backend records or executes immediately (CommandBuffer contract).
class VulkanCommandBuffer final : public CommandBuffer {
public:
    explicit VulkanCommandBuffer(VulkanDevice::Impl& device) : d_ { &device } {}

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
        // V1/V2 target the swapchain only (desc.framebuffer is ignored until
        // V4 creates real VkFramebuffers for offscreen targets).
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

    void copyBuffer(BufferHandle src, BufferHandle dst, u64 size, u64 srcOffset,
                    u64 dstOffset) override;
    void copyTexture(TextureHandle src, TextureHandle dst) override;

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
    void dispatch(u32, u32, u32) override {}
    void memoryBarrier() override {}

private:
    VulkanDevice::Impl* d_ { nullptr };
    VkCommandBuffer cb_ { VK_NULL_HANDLE };
    VkRenderPass pass_ { VK_NULL_HANDLE };
    VkFramebuffer framebuffer_ { VK_NULL_HANDLE };
    VkExtent2D extent_ {};
    bool inPass_ { false };
};

void VulkanCommandBuffer::copyBuffer(BufferHandle src, BufferHandle dst,
                                     u64 size, u64 srcOffset, u64 dstOffset) {
    VulkanBuffer* s = d_->findBuffer(src);
    VulkanBuffer* t = d_->findBuffer(dst);
    if (cb_ == VK_NULL_HANDLE || !s || !t) {
        return;
    }
    VkBufferCopy region {};
    region.srcOffset = srcOffset;
    region.dstOffset = dstOffset;
    region.size = size;
    vkCmdCopyBuffer(cb_, s->buffer, t->buffer, 1, &region);
}

void VulkanCommandBuffer::copyTexture(TextureHandle src, TextureHandle dst) {
    VulkanTexture* s = d_->findTexture(src);
    VulkanTexture* t = d_->findTexture(dst);
    if (cb_ == VK_NULL_HANDLE || !s || !t || inPass_) {
        return; // must be called outside a render pass (CommandBuffer contract)
    }
    const VkImageLayout srcWas = s->layout;
    const VkImageLayout dstWas = t->layout;
    transitionLayout(cb_, s->image, s->aspect, 0, 1, s->arrayLayers, srcWas,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transitionLayout(cb_, t->image, t->aspect, 0, 1, t->arrayLayers, dstWas,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageCopy region {};
    region.srcSubresource.aspectMask = s->aspect;
    region.srcSubresource.layerCount = s->arrayLayers;
    region.dstSubresource.aspectMask = t->aspect;
    region.dstSubresource.layerCount = t->arrayLayers;
    region.extent = s->extent;
    vkCmdCopyImage(cb_, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Both are sampled again afterwards (the point of snapshotting).
    transitionLayout(cb_, s->image, s->aspect, 0, 1, s->arrayLayers,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transitionLayout(cb_, t->image, t->aspect, 0, 1, t->arrayLayers,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    s->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

} // namespace

// --- Transfer helpers ----------------------------------------------------------

template <typename F>
bool VulkanDevice::Impl::immediateSubmit(F&& record) {
    VkCommandBufferAllocateInfo alloc {};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = transferPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (!vkOk(vkAllocateCommandBuffers(device, &alloc, &cb),
              "vkAllocateCommandBuffers(transfer)")) {
        return false;
    }

    VkCommandBufferBeginInfo begin {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);
    record(cb);
    vkEndCommandBuffer(cb);

    VkFenceCreateInfo fenceInfo {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(device, &fenceInfo, nullptr, &fence);

    VkSubmitInfo submit {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    const bool ok = vkOk(vkQueueSubmit(graphicsQueue, 1, &submit, fence),
                         "vkQueueSubmit(transfer)");
    if (ok) {
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    }
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, transferPool, 1, &cb);
    return ok;
}

bool VulkanDevice::Impl::createStaging(u64 size, bool forRead, VkBuffer& out,
                                       VmaAllocation& outAlloc,
                                       void** outMapped) {
    VkBufferCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = forRead ? VK_BUFFER_USAGE_TRANSFER_DST_BIT
                         : VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc {};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.flags =
        VMA_ALLOCATION_CREATE_MAPPED_BIT |
        (forRead ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                 : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);

    VmaAllocationInfo allocInfo {};
    if (!vkOk(vmaCreateBuffer(allocator, &info, &alloc, &out, &outAlloc,
                              &allocInfo),
              "vmaCreateBuffer(staging)")) {
        return false;
    }
    *outMapped = allocInfo.pMappedData;
    return true;
}

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

        for (auto& [id, sampler] : impl->samplers) {
            vkDestroySampler(impl->device, sampler, nullptr);
        }
        for (auto& [id, tex] : impl->textures) {
            vkDestroyImageView(impl->device, tex.view, nullptr);
            vmaDestroyImage(impl->allocator, tex.image, tex.allocation);
        }
        for (auto& [id, buf] : impl->buffers) {
            vmaDestroyBuffer(impl->allocator, buf.buffer, buf.allocation);
        }

        for (u32 i = 0; i < kFramesInFlight; ++i) {
            vkDestroySemaphore(impl->device, impl->imageAvailable[i], nullptr);
            vkDestroyFence(impl->device, impl->inFlight[i], nullptr);
        }
        impl->destroySwapchain();
        if (impl->commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(impl->device, impl->commandPool, nullptr);
        }
        if (impl->transferPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(impl->device, impl->transferPool, nullptr);
        }
        if (impl->allocator != nullptr) {
            vmaDestroyAllocator(impl->allocator);
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
        return *d.cmd;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        vkOk(acquired, "vkAcquireNextImageKHR");
        return *d.cmd;
    }

    vkResetFences(d.device, 1, &d.inFlight[d.frame]);
    vkResetCommandBuffer(d.commandBuffers[d.frame], 0);

    VkCommandBufferBeginInfo begin {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!vkOk(vkBeginCommandBuffer(d.commandBuffers[d.frame], &begin),
              "vkBeginCommandBuffer")) {
        return *d.cmd;
    }

    d.cmd->begin(d.commandBuffers[d.frame], d.renderPass,
                 d.framebuffers[d.imageIndex], d.extent);
    d.frameActive = true;
    return *d.cmd;
}

void VulkanDevice::endFrame() {
    Impl& d = *impl;
    if (!d.frameActive) {
        return; // acquire failed this frame — nothing was recorded
    }
    // A caller that forgot endRenderPass would leave the pass open; closing it
    // here keeps the command buffer valid rather than failing the submit.
    d.cmd->endRenderPass();

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

u64 VulkanDevice::nativeTextureId(TextureHandle) const {
    // V6: hand ImGui a VkDescriptorSet for the offscreen target.
    return 0;
}

// --- Buffers -------------------------------------------------------------------

BufferHandle VulkanDevice::createBuffer(const BufferDesc& desc,
                                        const void* initialData) {
    Impl& d = *impl;
    if (desc.size == 0) {
        LOG_ERROR("Vulkan createBuffer: zero size");
        return {};
    }
    // Host-visible when the caller rewrites it every frame or reads it back;
    // device-local otherwise, written through a staging copy.
    const bool hostVisible = desc.dynamic || desc.readback;

    VkBufferCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = desc.size;
    info.usage = toVkBufferUsage(desc.usage) |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc {};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostVisible) {
        alloc.flags =
            VMA_ALLOCATION_CREATE_MAPPED_BIT |
            (desc.readback
                 ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                 : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    }

    VulkanBuffer buffer {};
    buffer.size = desc.size;
    VmaAllocationInfo allocInfo {};
    if (!vkOk(vmaCreateBuffer(d.allocator, &info, &alloc, &buffer.buffer,
                              &buffer.allocation, &allocInfo),
              "vmaCreateBuffer")) {
        return {};
    }
    buffer.mapped = hostVisible ? allocInfo.pMappedData : nullptr;

    const u32 id = d.nextId++;
    d.buffers.emplace(id, buffer);
    if (initialData != nullptr) {
        updateBuffer({ id }, initialData, desc.size, 0);
    }
    return { id };
}

void VulkanDevice::updateBuffer(BufferHandle handle, const void* data, u64 size,
                                u64 offset) {
    Impl& d = *impl;
    VulkanBuffer* buffer = d.findBuffer(handle);
    if (!buffer || data == nullptr || size == 0) {
        return;
    }
    if (offset + size > buffer->size) {
        LOG_ERROR("Vulkan updateBuffer: range {}+{} exceeds size {}", offset,
                  size, buffer->size);
        return;
    }

    if (buffer->mapped != nullptr) {
        std::memcpy(static_cast<u8*>(buffer->mapped) + offset, data, size);
        vmaFlushAllocation(d.allocator, buffer->allocation, offset, size);
        return;
    }

    // Device-local: stage then copy on the GPU.
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = nullptr;
    void* mapped = nullptr;
    if (!d.createStaging(size, false, staging, stagingAlloc, &mapped)) {
        return;
    }
    std::memcpy(mapped, data, size);
    vmaFlushAllocation(d.allocator, stagingAlloc, 0, size);

    d.immediateSubmit([&](VkCommandBuffer cb) {
        VkBufferCopy region {};
        region.dstOffset = offset;
        region.size = size;
        vkCmdCopyBuffer(cb, staging, buffer->buffer, 1, &region);
    });
    vmaDestroyBuffer(d.allocator, staging, stagingAlloc);
}

void VulkanDevice::destroyBuffer(BufferHandle handle) {
    Impl& d = *impl;
    auto it = d.buffers.find(handle.id);
    if (it == d.buffers.end()) {
        return;
    }
    vmaDestroyBuffer(d.allocator, it->second.buffer, it->second.allocation);
    d.buffers.erase(it);
}

void VulkanDevice::readBuffer(BufferHandle handle, void* dst, u64 size,
                              u64 offset) {
    Impl& d = *impl;
    VulkanBuffer* buffer = d.findBuffer(handle);
    if (!buffer || dst == nullptr || size == 0) {
        return;
    }
    if (offset + size > buffer->size) {
        LOG_ERROR("Vulkan readBuffer: range {}+{} exceeds size {}", offset,
                  size, buffer->size);
        return;
    }

    if (buffer->mapped != nullptr) {
        vmaInvalidateAllocation(d.allocator, buffer->allocation, offset, size);
        std::memcpy(dst, static_cast<u8*>(buffer->mapped) + offset, size);
        return;
    }

    // Device-local: copy into a host-visible staging buffer, then read.
    // Callers that do this every frame should set BufferDesc::readback so the
    // buffer is host-visible to begin with (§ Rhi.hpp).
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = nullptr;
    void* mapped = nullptr;
    if (!d.createStaging(size, true, staging, stagingAlloc, &mapped)) {
        return;
    }
    d.immediateSubmit([&](VkCommandBuffer cb) {
        VkBufferCopy region {};
        region.srcOffset = offset;
        region.size = size;
        vkCmdCopyBuffer(cb, buffer->buffer, staging, 1, &region);
    });
    vmaInvalidateAllocation(d.allocator, stagingAlloc, 0, size);
    std::memcpy(dst, mapped, size);
    vmaDestroyBuffer(d.allocator, staging, stagingAlloc);
}

// --- Textures ------------------------------------------------------------------

TextureHandle VulkanDevice::createTexture(const TextureDesc& desc,
                                          const void* pixels) {
    Impl& d = *impl;
    if (desc.width == 0 || desc.height == 0) {
        LOG_ERROR("Vulkan createTexture: zero extent");
        return {};
    }
    if (desc.depth > 1 && desc.arrayLayers > 1) {
        LOG_ERROR("Vulkan createTexture: depth and arrayLayers are exclusive");
        return {};
    }

    const bool volume = desc.depth > 1;
    VulkanTexture tex {};
    tex.format = toVkFormat(desc.format);
    tex.extent = { desc.width, desc.height, volume ? desc.depth : 1u };
    tex.mipLevels = std::max(1u, desc.mipLevels);
    tex.arrayLayers = volume ? 1u : std::max(1u, desc.arrayLayers);
    tex.aspect = isDepthFormat(desc.format) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                            : VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageUsageFlags usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (desc.usage & TextureUsage_Sampled) {
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (desc.usage & TextureUsage_RenderAttachment) {
        usage |= isDepthFormat(desc.format)
                     ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                     : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    // Volumes are GPU-written through storage images (GI clipmap, cascades).
    if (volume) {
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }

    VkImageCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = tex.format;
    info.extent = tex.extent;
    info.mipLevels = tex.mipLevels;
    info.arrayLayers = tex.arrayLayers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc {};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    if (!vkOk(vmaCreateImage(d.allocator, &info, &alloc, &tex.image,
                             &tex.allocation, nullptr),
              "vmaCreateImage")) {
        return {};
    }

    VkImageViewCreateInfo view {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = tex.image;
    view.viewType = volume                ? VK_IMAGE_VIEW_TYPE_3D
                    : tex.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                          : VK_IMAGE_VIEW_TYPE_2D;
    view.format = tex.format;
    view.subresourceRange.aspectMask = tex.aspect;
    view.subresourceRange.levelCount = tex.mipLevels;
    view.subresourceRange.layerCount = tex.arrayLayers;
    if (!vkOk(vkCreateImageView(d.device, &view, nullptr, &tex.view),
              "vkCreateImageView(texture)")) {
        vmaDestroyImage(d.allocator, tex.image, tex.allocation);
        return {};
    }

    // Upload the base mip of every layer (tightly packed, layer-major).
    if (pixels != nullptr) {
        const u64 size = static_cast<u64>(desc.width) * desc.height *
                         tex.extent.depth * tex.arrayLayers *
                         bytesPerTexel(desc.format);
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAlloc = nullptr;
        void* mapped = nullptr;
        if (d.createStaging(size, false, staging, stagingAlloc, &mapped)) {
            std::memcpy(mapped, pixels, size);
            vmaFlushAllocation(d.allocator, stagingAlloc, 0, size);
            d.immediateSubmit([&](VkCommandBuffer cb) {
                transitionLayout(cb, tex.image, tex.aspect, 0, tex.mipLevels,
                                 tex.arrayLayers, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy region {};
                region.imageSubresource.aspectMask = tex.aspect;
                region.imageSubresource.mipLevel = 0;
                region.imageSubresource.baseArrayLayer = 0;
                region.imageSubresource.layerCount = tex.arrayLayers;
                region.imageExtent = tex.extent;
                vkCmdCopyBufferToImage(cb, staging, tex.image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                       &region);
                transitionLayout(cb, tex.image, tex.aspect, 0, tex.mipLevels,
                                 tex.arrayLayers,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            });
            vmaDestroyBuffer(d.allocator, staging, stagingAlloc);
            tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    } else {
        // Render targets and GPU-written volumes start empty; move them out of
        // UNDEFINED so the first barrier has a known source layout.
        const VkImageLayout initial =
            volume ? VK_IMAGE_LAYOUT_GENERAL
                   : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        d.immediateSubmit([&](VkCommandBuffer cb) {
            transitionLayout(cb, tex.image, tex.aspect, 0, tex.mipLevels,
                             tex.arrayLayers, VK_IMAGE_LAYOUT_UNDEFINED,
                             initial);
        });
        tex.layout = initial;
    }

    const u32 id = d.nextId++;
    d.textures.emplace(id, tex);
    return { id };
}

void VulkanDevice::destroyTexture(TextureHandle handle) {
    Impl& d = *impl;
    auto it = d.textures.find(handle.id);
    if (it == d.textures.end()) {
        return;
    }
    vkDestroyImageView(d.device, it->second.view, nullptr);
    vmaDestroyImage(d.allocator, it->second.image, it->second.allocation);
    d.textures.erase(it);
}

void VulkanDevice::generateMipmaps(TextureHandle handle) {
    Impl& d = *impl;
    VulkanTexture* tex = d.findTexture(handle);
    if (!tex || tex->mipLevels <= 1) {
        return;
    }
    // Blitting requires the format to support linear filtering.
    VkFormatProperties props {};
    vkGetPhysicalDeviceFormatProperties(d.gpu, tex->format, &props);
    if (!(props.optimalTilingFeatures &
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        LOG_ERROR("Vulkan generateMipmaps: format has no linear blit support");
        return;
    }

    const VkImageLayout was = tex->layout;
    d.immediateSubmit([&](VkCommandBuffer cb) {
        // Level 0 becomes the blit source; the rest are transfer destinations.
        transitionLayout(cb, tex->image, tex->aspect, 0, 1, tex->arrayLayers,
                         was, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transitionLayout(cb, tex->image, tex->aspect, 1, tex->mipLevels - 1,
                         tex->arrayLayers, was,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        i32 width = static_cast<i32>(tex->extent.width);
        i32 height = static_cast<i32>(tex->extent.height);
        i32 depth = static_cast<i32>(tex->extent.depth);
        for (u32 level = 1; level < tex->mipLevels; ++level) {
            const i32 nextW = std::max(1, width / 2);
            const i32 nextH = std::max(1, height / 2);
            const i32 nextD = std::max(1, depth / 2);

            VkImageBlit blit {};
            blit.srcSubresource.aspectMask = tex->aspect;
            blit.srcSubresource.mipLevel = level - 1;
            blit.srcSubresource.layerCount = tex->arrayLayers;
            blit.srcOffsets[1] = { width, height, depth };
            blit.dstSubresource.aspectMask = tex->aspect;
            blit.dstSubresource.mipLevel = level;
            blit.dstSubresource.layerCount = tex->arrayLayers;
            blit.dstOffsets[1] = { nextW, nextH, nextD };
            vkCmdBlitImage(cb, tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_LINEAR);

            // This level becomes the next iteration's source.
            transitionLayout(cb, tex->image, tex->aspect, level, 1,
                             tex->arrayLayers,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            width = nextW;
            height = nextH;
            depth = nextD;
        }
        // Every level is now TRANSFER_SRC; leave the whole image sampleable.
        transitionLayout(cb, tex->image, tex->aspect, 0, tex->mipLevels,
                         tex->arrayLayers,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// --- Samplers ------------------------------------------------------------------

SamplerHandle VulkanDevice::createSampler(const SamplerDesc& desc) {
    Impl& d = *impl;
    VkSamplerCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.minFilter = toVkFilter(desc.minFilter);
    info.magFilter = toVkFilter(desc.magFilter);
    info.mipmapMode = desc.mipmapFilter ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                        : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = toVkAddressMode(desc.addressU);
    info.addressModeV = toVkAddressMode(desc.addressV);
    info.addressModeW = toVkAddressMode(desc.addressW);
    info.maxLod = VK_LOD_CLAMP_NONE;
    if (desc.maxAnisotropy > 1.0f) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = desc.maxAnisotropy;
    }
    // CompareFunc::Never means "not a comparison sampler" (§ Rhi.hpp), which
    // is why it maps to disabled rather than to VK_COMPARE_OP_NEVER.
    if (desc.compare != CompareFunc::Never) {
        info.compareEnable = VK_TRUE;
        info.compareOp = toVkCompareOp(desc.compare);
    }

    VkSampler sampler = VK_NULL_HANDLE;
    if (!vkOk(vkCreateSampler(d.device, &info, nullptr, &sampler),
              "vkCreateSampler")) {
        return {};
    }
    const u32 id = d.nextId++;
    d.samplers.emplace(id, sampler);
    return { id };
}

void VulkanDevice::destroySampler(SamplerHandle handle) {
    Impl& d = *impl;
    auto it = d.samplers.find(handle.id);
    if (it == d.samplers.end()) {
        return;
    }
    vkDestroySampler(d.device, it->second, nullptr);
    d.samplers.erase(it);
}

// --- Pipelines / bind groups / queries (V3, V4, V6) ---------------------------

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
    if (hasExtension(availableExt,
                     VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
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
        const vector<VkExtensionProperties> devExtProps =
            deviceExtensionProperties(candidate);
        if (!hasExtension(devExtProps, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
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
        VkPhysicalDeviceProperties candidateProps {};
        vkGetPhysicalDeviceProperties(candidate, &candidateProps);
        // Prefer a discrete GPU; on the M1 there is only the integrated one.
        const i32 score =
            candidateProps.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                ? 2
                : 1;
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
    VkPhysicalDeviceFeatures features {};
    vkGetPhysicalDeviceFeatures(d.gpu, &features);

    // --- Logical device ---
    vector<const char*> deviceExtensions { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    const vector<VkExtensionProperties> devExt =
        deviceExtensionProperties(d.gpu);
    // Mandatory when present (MoltenVK): the spec requires enabling it on a
    // portability driver.
    if (hasExtension(devExt, "VK_KHR_portability_subset")) {
        deviceExtensions.push_back("VK_KHR_portability_subset");
    }

    // Only anisotropy is requested, and only if the GPU has it — samplers fall
    // back to isotropic filtering otherwise.
    VkPhysicalDeviceFeatures enabled {};
    enabled.samplerAnisotropy = features.samplerAnisotropy;

    const f32 priority = 1.0f;
    vector<VkDeviceQueueCreateInfo> queueInfos;
    VkDeviceQueueCreateInfo graphicsQueueInfo {};
    graphicsQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    graphicsQueueInfo.queueFamilyIndex = d.graphicsFamily;
    graphicsQueueInfo.queueCount = 1;
    graphicsQueueInfo.pQueuePriorities = &priority;
    queueInfos.push_back(graphicsQueueInfo);
    if (d.presentFamily != d.graphicsFamily) {
        VkDeviceQueueCreateInfo presentQueueInfo = graphicsQueueInfo;
        presentQueueInfo.queueFamilyIndex = d.presentFamily;
        queueInfos.push_back(presentQueueInfo);
    }

    VkDeviceCreateInfo deviceInfo {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = static_cast<u32>(queueInfos.size());
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.enabledExtensionCount = static_cast<u32>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    deviceInfo.pEnabledFeatures = &enabled;
    if (!vkOk(vkCreateDevice(d.gpu, &deviceInfo, nullptr, &d.device),
              "vkCreateDevice")) {
        return nullptr;
    }
    vkGetDeviceQueue(d.device, d.graphicsFamily, 0, &d.graphicsQueue);
    vkGetDeviceQueue(d.device, d.presentFamily, 0, &d.presentQueue);

    // --- Allocator ---
    VmaAllocatorCreateInfo allocatorInfo {};
    allocatorInfo.physicalDevice = d.gpu;
    allocatorInfo.device = d.device;
    allocatorInfo.instance = d.instance;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_2;
    if (!vkOk(vmaCreateAllocator(&allocatorInfo, &d.allocator),
              "vmaCreateAllocator")) {
        return nullptr;
    }

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
    VkCommandPoolCreateInfo transferPoolInfo = poolInfo;
    transferPoolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (!vkOk(vkCreateCommandPool(d.device, &transferPoolInfo, nullptr,
                                  &d.transferPool),
              "vkCreateCommandPool(transfer)")) {
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

    d.cmd = std::make_unique<VulkanCommandBuffer>(d);

    // Caps stay all-false until pipelines exist (V4): renderer systems gate on
    // these flags to decide whether to run, and every draw path they would
    // take is still a no-op. Resources (V2) work, but nothing can draw with
    // them yet — they are flipped on as a set when V4 lands.
    LOG_INFO("Vulkan device ready: {} — {}x{}, {} swapchain images "
             "(V2: resources, no pipelines yet)",
             props.deviceName, d.extent.width, d.extent.height,
             static_cast<u32>(d.images.size()));
    return self;
}

uptr<Device> createVulkanDevice(platform::Window& window) {
    return VulkanDevice::create(window);
}

} // namespace rhi
