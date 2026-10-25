#include "engine/rhi/backends/vulkan/VulkanInternal.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include <glm/gtc/packing.hpp> // f32 -> f16 for R16F initial-data uploads

#include "engine/core/Assert.hpp"
#include "engine/core/Hash.hpp"
#include "engine/core/Log.hpp"

namespace rhi {

// --- Format / state conversion ----------------------------------------------

VkFormat toVkFormat(TextureFormat format) {
    switch (format) {
    case TextureFormat::RGBA8:     return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::SRGBA8:    return VK_FORMAT_R8G8B8A8_SRGB;
    case TextureFormat::RGBA16F:   return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TextureFormat::R16F:      return VK_FORMAT_R16_SFLOAT;
    case TextureFormat::R32F:      return VK_FORMAT_R32_SFLOAT;
    case TextureFormat::Depth32F:  return VK_FORMAT_D32_SFLOAT;
    case TextureFormat::BC7_SRGB:  return VK_FORMAT_BC7_SRGB_BLOCK;
    case TextureFormat::BC7_UNORM: return VK_FORMAT_BC7_UNORM_BLOCK;
    case TextureFormat::BC5_UNORM: return VK_FORMAT_BC5_UNORM_BLOCK;
    case TextureFormat::R16_UNORM: return VK_FORMAT_R16_UNORM;
    case TextureFormat::Count: break;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
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
    case BufferUsage::Indirect:
        // SSBO-writable by design: the cull dispatch fills the commands.
        return VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
}

// Identifies a render target by its attachment formats, which is exactly what
// pipeline compatibility depends on under dynamic rendering.
u64 targetKey(const vector<VkFormat>& colorFormats, VkFormat depthFormat) {
    u64 key = core::kFnv64Basis;
    auto mix = [&key](u64 value) {
        key = (key ^ value) * core::kFnv64Prime;
    };
    for (VkFormat f : colorFormats) {
        mix(static_cast<u64>(f));
    }
    mix(static_cast<u64>(depthFormat) ^ 0x9e3779b9ull);
    return key;
}

// --- Buffers -------------------------------------------------------------------

namespace {

// The stages/accesses that can READ a buffer of this declared usage — the
// scope of recordHostUpdate's two barriers.
void bufferReadScope(BufferUsage usage, VkPipelineStageFlags& stages,
                     VkAccessFlags& access) {
    switch (usage) {
    case BufferUsage::Indirect:
        stages = VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        access = VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                 VK_ACCESS_SHADER_READ_BIT;
        return;
    case BufferUsage::Vertex:
        stages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        access = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        return;
    case BufferUsage::Index:
        stages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        access = VK_ACCESS_INDEX_READ_BIT;
        return;
    case BufferUsage::Uniform:
        stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        access = VK_ACCESS_UNIFORM_READ_BIT;
        return;
    case BufferUsage::Storage:
        stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        access = VK_ACCESS_SHADER_READ_BIT;
        return;
    }
}

// Stages `data` through the frame's staging ring and records the copy into
// the currently recording command buffer (the compute chain during a
// routing window, else the frame's). False when nothing is recording, a
// pass is open, or the ring is full — the caller falls back.
bool stageIntoFrameCb(VulkanDevice::Impl& d, VulkanBuffer& buffer,
                      const void* data, u64 size, u64 offset) {
    VulkanCommandBuffer* rec =
        d.computeRouting ? d.computeCmd.get() : d.cmd.get();
    if (!d.frameActive || rec == nullptr || rec->insidePass()) {
        return false;
    }
    VkPipelineStageFlags readStages = 0;
    VkAccessFlags readAccess = 0;
    bufferReadScope(buffer.usage, readStages, readAccess);
    VkBuffer staging = VK_NULL_HANDLE;
    u64 srcOffset = 0;
    void* mapped = nullptr;
    if (!d.ringAlloc(size, staging, srcOffset, &mapped)) {
        return false;
    }
    std::memcpy(mapped, data, size);
    vmaFlushAllocation(d.allocator, d.stagingRings[d.frame].allocation,
                       srcOffset, size);
    return rec->recordHostUpdate(staging, srcOffset, buffer.buffer, size,
                                 offset, readStages, readAccess);
}

} // namespace

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
    // With async compute, any buffer may be read on the second queue (and
    // written from the upload queue) — CONCURRENT skips per-resource
    // ownership transfers (prototype simplicity over the minor bandwidth
    // cost; Metal ignores it).
    const std::array<u32, 3> shareFamilies { d.graphicsFamily,
                                             d.computeFamily,
                                             d.transferFamily };
    if (d.asyncComputeAvailable) {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = d.uploadQueueAvailable ? 3 : 2;
        info.pQueueFamilyIndices = shareFamilies.data();
    }

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
    buffer.usage = desc.usage;
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
        // The in-place memcpy races the frame still in flight
        // (kFramesInFlight > 1): its passes then read values meant for the
        // NEXT frame — the CSM matrices flipped mid-frame and cast
        // flickering dark plates on fast pans (GL is immune,
        // the driver versions buffer updates). While recording, route the
        // update through a staged copy in the frame's own command buffer.
        // In-place remains for init/tools (nothing consuming yet) and
        // mid-pass updates (copies are illegal inside a pass — ImGui's
        // vertex streams, which tolerate the race).
        // Async recording routes the copy into the COMPUTE command
        // buffer: it executes on the compute queue, ordered with the
        // chain that reads it — and never races the next frame's
        // graphics copies (docs/RENDERING.md PG3).
        if (stageIntoFrameCb(d, *buffer, data, size, offset)) {
            return; // ring slice — reclaimed when the slot cycles
        }
        std::memcpy(static_cast<u8*>(buffer->mapped) + offset, data, size);
        vmaFlushAllocation(d.allocator, buffer->allocation, offset, size);
        return;
    }

    // Device-local: stage then copy on the GPU. With the upload queue on,
    // the copy records into the frame's TRANSFER command buffer — off the
    // graphics stream entirely; the endFrame submit chain orders it before
    // every reader (contract: the whole frame sees the NEW data). Inside a
    // compute-routing window the update belongs to the compute chain's own
    // stream and stays there.
    if (d.uploadQueueAvailable && d.frameActive && !d.computeRouting &&
        d.recordUpload(buffer->buffer, data, size, offset)) {
        return;
    }
    // Fallback: the FRAME command buffer (recordHostUpdate) from a ring
    // slice — same mechanism as the dynamic-UBO path. A bare async submit
    // (immediateSubmit(wait=false)) carries no ordering against the
    // frame's own draws: sync validation caught the frame reading vertex
    // buffers still being written by the transfer (READ_AFTER_WRITE).
    if (stageIntoFrameCb(d, *buffer, data, size, offset)) {
        return;
    }
    // Outside a frame (init, tools): nothing ever waits, block as before
    // with a dedicated staging.
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = nullptr;
    void* mapped = nullptr;
    if (!d.createStaging(size, false, staging, stagingAlloc, &mapped)) {
        return;
    }
    std::memcpy(mapped, data, size);
    vmaFlushAllocation(d.allocator, stagingAlloc, 0, size);
    d.immediateSubmit(
        [&](VkCommandBuffer cb) {
            VkBufferCopy region {};
            region.dstOffset = offset;
            region.size = size;
            vkCmdCopyBuffer(cb, staging, buffer->buffer, 1, &region);
        },
        /*wait=*/true);
    vmaDestroyBuffer(d.allocator, staging, stagingAlloc);
}

// Destroying a resource still referenced by an in-flight command buffer is
// undefined. During a frame the resource is parked in the deferred-free
// queue (released once its frame slot recycles); out of frame nothing
// tracks in-flight use, so a full drain first is the safe move — those
// paths (hot reload, teardown, resize) are rare and never per-frame.
void VulkanDevice::destroyBuffer(BufferHandle handle) {
    Impl& d = *impl;
    auto it = d.buffers.find(handle.id);
    if (it == d.buffers.end()) {
        return;
    }
    // ONE destruction policy (in-frame or not): park until the frame
    // slots cycle past every submitted or still-recording reference —
    // never a device-wide stall in a destroy.
    d.pendingBuffers.push_back({ it->second.buffer, it->second.allocation,
                                 d.frameCounter });
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

namespace {

// How a texture's initial pixels reach the GPU, decided before any Vulkan
// call is made: staging size, copy regions, the conversion R16F needs, and
// whether the per-frame upload ring may carry it. Execution is one of the
// two uploaders below.
struct ImageUploadPlan {
    u64 size { 0 };       // staging bytes
    u64 texelCount { 0 }; // base-mip texels across layers (conversion loop)
    vector<VkBufferImageCopy> regions;
    // RHI contract (matches what the GL driver does with GL_FLOAT uploads):
    // R16F initial data is packed f32 per texel, converted on the way into
    // the staging. A raw memcpy would reinterpret f32 halves as f16 — the
    // bug that fed the GI terrain tile (and the water pool map) garbage
    // heights.
    bool convertF32ToF16 { false };
    // May ride the upload queue's ring + command buffer (zero submits, zero
    // waits). Runtime-mip textures may NOT: their generateMipmaps blits
    // submit immediately, while the upload CB lands at endFrame — the ring
    // would generate the chain from garbage (the magenta ferns/rocks).
    // R16F may not either: its conversion needs a dedicated staging.
    bool ringEligible { false };
    // Created with pixelsIncludeMips: the offline chain is authoritative.
    bool offlineMips { false };
};

ImageUploadPlan planImageUpload(const TextureDesc& desc,
                                const VulkanTexture& tex) {
    ImageUploadPlan plan;
    if (desc.pixelsIncludeMips) {
        // The full offline mip chain (mip-major, layers contiguous per mip
        // — see TextureDesc::pixelsIncludeMips): one copy region per mip
        // covering every layer.
        plan.size = textureDataBytes(desc);
        plan.regions.reserve(tex.mipLevels);
        u64 offset = 0;
        u32 w = desc.width;
        u32 h = desc.height;
        for (u32 mip = 0; mip < tex.mipLevels; ++mip) {
            // bufferOffset must be a multiple of the texel block size
            // (and 4); holds by construction for BC (16-byte blocks)
            // and R16_UNORM (power-of-two mip areas).
            ENGINE_ASSERT(offset % std::max<u64>(
                              4, formatBlockBytes(desc.format)) == 0);
            VkBufferImageCopy region {};
            region.bufferOffset = offset;
            region.imageSubresource.aspectMask = tex.aspect;
            region.imageSubresource.mipLevel = mip;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = tex.arrayLayers;
            region.imageExtent = { w, h, 1 };
            plan.regions.push_back(region);
            offset += mipLevelBytes(desc.format, w, h) * tex.arrayLayers;
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        plan.ringEligible = true;
        plan.offlineMips = true;
        return plan;
    }
    // The base mip of every layer (tightly packed, layer-major).
    plan.texelCount = static_cast<u64>(desc.width) * desc.height *
                      tex.extent.depth * tex.arrayLayers;
    plan.size = plan.texelCount * formatGpuBytes(desc.format);
    VkBufferImageCopy baseRegion {};
    baseRegion.imageSubresource.aspectMask = tex.aspect;
    baseRegion.imageSubresource.mipLevel = 0;
    baseRegion.imageSubresource.baseArrayLayer = 0;
    baseRegion.imageSubresource.layerCount = tex.arrayLayers;
    baseRegion.imageExtent = tex.extent;
    plan.regions.push_back(baseRegion);
    plan.convertF32ToF16 = desc.format == TextureFormat::R16F;
    plan.ringEligible = !plan.convertF32ToF16 && tex.mipLevels == 1;
    return plan;
}

// In-frame path: the upload queue's ring + command buffer (recordImageUpload
// decides whether a frame is recording and the lane is on).
bool uploadImageViaRing(VulkanDevice::Impl& d, const VulkanTexture& tex,
                        const ImageUploadPlan& plan, const void* pixels) {
    return d.recordImageUpload(tex.image, tex.aspect, tex.mipLevels,
                               tex.arrayLayers, pixels, plan.size,
                               plan.regions);
}

// Dedicated staging + async transfer submit (outside a frame — init, tools
// — or when the ring is not an option), parked in the pendings until the
// slot cycles past the copy. Its queue order with later generateMipmaps
// blits is guaranteed.
bool uploadImageImmediate(VulkanDevice::Impl& d, const VulkanTexture& tex,
                          const ImageUploadPlan& plan, const void* pixels) {
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = nullptr;
    void* mapped = nullptr;
    if (!d.createStaging(plan.size, false, staging, stagingAlloc, &mapped)) {
        return false;
    }
    if (plan.convertF32ToF16) {
        const f32* src = static_cast<const f32*>(pixels);
        u16* dst = static_cast<u16*>(mapped);
        for (u64 i = 0; i < plan.texelCount; ++i) {
            dst[i] = glm::packHalf1x16(src[i]);
        }
    } else {
        std::memcpy(mapped, pixels, plan.size);
    }
    vmaFlushAllocation(d.allocator, stagingAlloc, 0, plan.size);
    d.immediateSubmit(
        [&](VkCommandBuffer cb) {
            transitionLayout(cb, tex.image, tex.aspect, 0, tex.mipLevels,
                             tex.arrayLayers, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdCopyBufferToImage(cb, staging, tex.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<u32>(plan.regions.size()),
                                   plan.regions.data());
            transitionLayout(cb, tex.image, tex.aspect, 0, tex.mipLevels,
                             tex.arrayLayers,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        },
        /*wait=*/false);
    d.pendingBuffers.push_back({ staging, stagingAlloc, d.asyncParkFrame() });
    return true;
}

} // namespace

TextureHandle VulkanDevice::createTexture(const TextureDesc& desc,
                                          const void* pixels) {
    Impl& d = *impl;
    if (!validateTextureDesc(desc, pixels != nullptr, caps())) {
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

    // SAMPLED unconditionally: in GL every texture is samplable, and the
    // renderer relies on it (shadow depth, scene depth, Hi-Z source are all
    // sampled whatever their declared usage). endRenderPass counts on it too
    // (attachments hand back as SHADER_READ_ONLY).
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc.usage & TextureUsage_RenderAttachment) {
        usage |= isDepthFormat(desc.format)
                     ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                     : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    // STORAGE wherever the format allows it (GL parity: compute writes 2D
    // targets too — Hi-Z mips, snow mask, cloud bake — not just volumes).
    if (formatTraits(desc.format).storage) {
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
    // Same CONCURRENT policy as buffers (async compute reads/writes, the
    // upload queue writes): an EXCLUSIVE image touched from the upload
    // family without an ownership transfer is undefined.
    const std::array<u32, 3> shareFamilies { d.graphicsFamily,
                                             d.computeFamily,
                                             d.transferFamily };
    if (d.asyncComputeAvailable) {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = d.uploadQueueAvailable ? 3 : 2;
        info.pQueueFamilyIndices = shareFamilies.data();
    }

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

    if (pixels != nullptr) {
        // Ring when the plan allows it and a frame is recording on the
        // upload lane; the immediate submit otherwise. A failed staging
        // leaves the image UNDEFINED and unflagged, as a never-uploaded one.
        const ImageUploadPlan plan = planImageUpload(desc, tex);
        const bool uploaded =
            (plan.ringEligible && uploadImageViaRing(d, tex, plan, pixels)) ||
            uploadImageImmediate(d, tex, plan, pixels);
        if (uploaded) {
            tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            tex.offlineMips = plan.offlineMips;
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

    // The creation-time sampler for sampler-less bind entries (legacy 2D).
    VkSamplerCreateInfo samplerInfo {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.minFilter = toVkFilter(desc.filter);
    samplerInfo.magFilter = toVkFilter(desc.filter);
    samplerInfo.mipmapMode = desc.mipLevels > 1
                                 ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                 : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = toVkAddressMode(desc.wrap);
    samplerInfo.addressModeV = toVkAddressMode(desc.wrap);
    samplerInfo.addressModeW = toVkAddressMode(desc.wrap);
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    vkOk(vkCreateSampler(d.device, &samplerInfo, nullptr, &tex.defaultSampler),
         "vkCreateSampler (texture default)");

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
    d.pendingTextures.push_back({ it->second.image, it->second.allocation,
                                  it->second.view,
                                  it->second.defaultSampler,
                                  it->second.mipViews,
                                  d.frameCounter });
    d.textures.erase(it);
}

void VulkanDevice::generateMipmaps(TextureHandle handle) {
    Impl& d = *impl;
    VulkanTexture* tex = d.findTexture(handle);
    if (!tex || tex->mipLevels <= 1) {
        return;
    }
    if (tex->offlineMips) {
        LOG_ERROR("Vulkan generateMipmaps: texture carries an offline mip "
                  "chain (pixelsIncludeMips) — refusing to overwrite it");
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
    // Async like the upload itself (no staging here — blits are
    // image-to-image); the barriers order every later read on the queue.
    d.immediateSubmit(
        [&](VkCommandBuffer cb) {
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
        },
        /*wait=*/false);
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
    d.pendingSamplers.push_back({ it->second, d.frameCounter });
    d.samplers.erase(it);
}

// --- Pipelines / bind groups / queries -----------------------------------------

// --- Render targets -------------------------------------------------------

FramebufferHandle VulkanDevice::createFramebuffer(const FramebufferDesc& desc) {
    Impl& d = *impl;
    VulkanFramebuffer target {};

    // A FramebufferAttachment selects one mip of one layer, so each needs its
    // own view rather than the texture's whole-resource view.
    auto makeView = [&](const FramebufferAttachment& attachment,
                        VkImageView& out, VkFormat& format,
                        u32& width, u32& height) {
        VulkanTexture* tex = d.findTexture(attachment.texture);
        if (tex == nullptr) {
            return false;
        }
        VkImageViewCreateInfo info {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = tex->image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = tex->format;
        info.subresourceRange.aspectMask = tex->aspect;
        info.subresourceRange.baseMipLevel = attachment.mipLevel;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.baseArrayLayer = attachment.arrayLayer;
        info.subresourceRange.layerCount = 1;
        if (!vkOk(vkCreateImageView(d.device, &info, nullptr, &out),
                  "vkCreateImageView(attachment)")) {
            return false;
        }
        format = tex->format;
        width = std::max(1u, tex->extent.width >> attachment.mipLevel);
        height = std::max(1u, tex->extent.height >> attachment.mipLevel);
        return true;
    };

    // Failure after a partial build must release the views already created.
    const auto bail = [&]() -> FramebufferHandle {
        for (VkImageView view : target.colorViews) {
            vkDestroyImageView(d.device, view, nullptr);
        }
        if (target.depthView != VK_NULL_HANDLE) {
            vkDestroyImageView(d.device, target.depthView, nullptr);
        }
        return {};
    };

    u32 width = 0;
    u32 height = 0;
    for (const FramebufferAttachment& attachment : desc.colorAttachments) {
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        if (!makeView(attachment, view, format, width, height)) {
            LOG_ERROR("Vulkan createFramebuffer: bad color attachment");
            return bail();
        }
        target.colorViews.push_back(view);
        target.colorFormats.push_back(format);
        target.colorTextures.push_back(attachment.texture);
    }
    if (desc.depthAttachment.texture.id != 0) {
        if (!makeView(desc.depthAttachment, target.depthView,
                      target.depthFormat, width, height)) {
            LOG_ERROR("Vulkan createFramebuffer: bad depth attachment");
            return bail();
        }
        target.depthTexture = desc.depthAttachment.texture;
    }
    if (width == 0 || height == 0) {
        LOG_ERROR("Vulkan createFramebuffer: no attachments");
        return bail();
    }
    target.extent = { width, height };

    const u32 id = d.nextId++;
    d.targets.emplace(id, std::move(target));
    return { id };
}

void VulkanDevice::destroyFramebuffer(FramebufferHandle handle) {
    Impl& d = *impl;
    auto it = d.targets.find(handle.id);
    if (it == d.targets.end()) {
        return;
    }
    d.pendingTargets.push_back({ it->second.colorViews,
                                 it->second.depthView, d.frameCounter });
    d.targets.erase(it);
}

// --- Pipelines --------------------------------------------------------------

namespace {

VkPrimitiveTopology toVkTopology(PrimitiveTopology topology) {
    switch (topology) {
    case PrimitiveTopology::Triangles:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PrimitiveTopology::TriangleStrip:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PrimitiveTopology::Lines: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkFormat toVkVertexFormat(VertexFormat format) {
    switch (format) {
    case VertexFormat::F32x1: return VK_FORMAT_R32_SFLOAT;
    case VertexFormat::F32x2: return VK_FORMAT_R32G32_SFLOAT;
    case VertexFormat::F32x3: return VK_FORMAT_R32G32B32_SFLOAT;
    case VertexFormat::F32x4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return VK_FORMAT_R32G32B32A32_SFLOAT;
}

VkCullModeFlags toVkCullMode(CullMode cull) {
    switch (cull) {
    case CullMode::None:  return VK_CULL_MODE_NONE;
    case CullMode::Back:  return VK_CULL_MODE_BACK_BIT;
    case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
    }
    return VK_CULL_MODE_NONE;
}

void applyBlend(BlendMode mode, VkPipelineColorBlendAttachmentState& state) {
    state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    state.blendEnable = mode == BlendMode::Opaque ? VK_FALSE : VK_TRUE;
    state.colorBlendOp = VK_BLEND_OP_ADD;
    state.alphaBlendOp = VK_BLEND_OP_ADD;
    state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    switch (mode) {
    case BlendMode::Opaque:
        break;
    case BlendMode::Alpha:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::Additive:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case BlendMode::PremultipliedAlpha:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    }
}

} // namespace

VkPipeline VulkanDevice::Impl::pipelineFor(VulkanPipeline& pipeline,
                                           const vector<VkFormat>& colorFormats,
                                           VkFormat depthFormat,
                                           VkFrontFace frontFace) {
    const u64 key = targetKey(colorFormats, depthFormat) ^
                    (frontFace == VK_FRONT_FACE_CLOCKWISE ? 0x5bf03635ull : 0);
    auto cached = pipeline.variants.find(key);
    if (cached != pipeline.variants.end()) {
        return cached->second;
    }

    VulkanShader* shader = findShader(pipeline.desc.shader);
    if (shader == nullptr || shader->vertex == VK_NULL_HANDLE) {
        LOG_ERROR("Vulkan pipeline: shader has no graphics stages");
        return VK_NULL_HANDLE;
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = shader->vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = shader->fragment;
    stages[1].pName = "main";

    vector<VkVertexInputBindingDescription> bindings;
    vector<VkVertexInputAttributeDescription> attributes;
    for (size_t slot = 0; slot < pipeline.desc.vertexBuffers.size(); ++slot) {
        const VertexBufferLayout& layout = pipeline.desc.vertexBuffers[slot];
        VkVertexInputBindingDescription binding {};
        binding.binding = static_cast<u32>(slot);
        binding.stride = layout.stride;
        binding.inputRate = layout.stepMode == VertexStepMode::Instance
                                ? VK_VERTEX_INPUT_RATE_INSTANCE
                                : VK_VERTEX_INPUT_RATE_VERTEX;
        bindings.push_back(binding);
        for (const VertexAttribute& attr : layout.attributes) {
            VkVertexInputAttributeDescription a {};
            a.location = attr.location;
            a.binding = static_cast<u32>(slot);
            a.format = toVkVertexFormat(attr.format);
            a.offset = attr.offset;
            attributes.push_back(a);
        }
    }

    VkPipelineVertexInputStateCreateInfo vertexInput {};
    vertexInput.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = static_cast<u32>(bindings.size());
    vertexInput.pVertexBindingDescriptions = bindings.data();
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<u32>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo assembly {};
    assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = toVkTopology(pipeline.desc.topology);

    VkPipelineViewportStateCreateInfo viewportState {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster {};
    raster.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = pipeline.desc.wireframe ? VK_POLYGON_MODE_LINE
                                                 : VK_POLYGON_MODE_FILL;
    raster.cullMode = toVkCullMode(pipeline.desc.cull);
    // Already the EFFECTIVE winding (per-target mirror folded in by
    // effectiveFrontFace() at the call sites).
    raster.frontFace = frontFace;
    raster.lineWidth = 1.0f;
    if (pipeline.desc.depthBias != 0.0f ||
        pipeline.desc.depthBiasSlope != 0.0f) {
        raster.depthBiasEnable = VK_TRUE;
        raster.depthBiasConstantFactor = pipeline.desc.depthBias;
        raster.depthBiasSlopeFactor = pipeline.desc.depthBiasSlope;
    }

    VkPipelineMultisampleStateCreateInfo multisample {};
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil {};
    depthStencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable =
        pipeline.desc.depth.testEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable =
        pipeline.desc.depth.writeEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = toVkCompareOp(pipeline.desc.depth.compare);

    vector<VkPipelineColorBlendAttachmentState> blendStates(
        std::max<size_t>(1, colorFormats.size()));
    for (VkPipelineColorBlendAttachmentState& state : blendStates) {
        applyBlend(pipeline.desc.blend, state);
    }
    VkPipelineColorBlendStateCreateInfo blend {};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = static_cast<u32>(colorFormats.size());
    blend.pAttachments = blendStates.data();

    const std::array<VkDynamicState, 2> dynamicStates {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamic {};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<u32>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();

    // Dynamic rendering replaces the render pass: the pipeline is told the
    // attachment formats directly.
    VkPipelineRenderingCreateInfo rendering {};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = static_cast<u32>(colorFormats.size());
    rendering.pColorAttachmentFormats = colorFormats.data();
    rendering.depthAttachmentFormat = depthFormat;

    VkGraphicsPipelineCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages.data();
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipeline.layout;

    VkPipeline vk = VK_NULL_HANDLE;
    if (!vkOk(vkCreateGraphicsPipelines(device, pipelineCache, 1, &info,
                                        nullptr, &vk),
              "vkCreateGraphicsPipelines")) {
        return VK_NULL_HANDLE;
    }
    pipeline.variants.emplace(key, vk);
    return vk;
}

namespace {

// Builds the descriptor set layout + pipeline layout from the shader's
// reflection. PUSH_DESCRIPTOR_BIT is what lets setBindGroup write through it
// without ever allocating a VkDescriptorSet.
bool buildLayouts(VkDevice device, VkSampler pcfSampler,
                  const VulkanShader& shader, VulkanPipeline& pipeline) {
    pipeline.resources = shader.resources;
    vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(shader.resources.size());
    for (const ShaderResource& resource : shader.resources) {
        VkDescriptorSetLayoutBinding binding {};
        binding.binding = resource.binding;
        binding.descriptorType = toVkDescriptorType(resource.klass);
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_ALL;
        if (resource.comparison && pcfSampler != VK_NULL_HANDLE) {
            // Immutable: MoltenVK cannot take comparison samplers as pushed
            // descriptors (mutableComparisonSamplers unsupported on M1).
            binding.pImmutableSamplers = &pcfSampler;
            if (resource.binding < 64) {
                pipeline.comparisonMask |= 1ull << resource.binding;
            }
        }
        bindings.push_back(binding);
        if (resource.binding < 64) {
            pipeline.bindingMask |= 1ull << resource.binding;
        }
    }

    VkDescriptorSetLayoutCreateInfo setInfo {};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    setInfo.bindingCount = static_cast<u32>(bindings.size());
    setInfo.pBindings = bindings.data();
    if (!vkOk(vkCreateDescriptorSetLayout(device, &setInfo, nullptr,
                                          &pipeline.setLayout),
              "vkCreateDescriptorSetLayout")) {
        return false;
    }

    VkPipelineLayoutCreateInfo layoutInfo {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &pipeline.setLayout;
    // The range must be declared here: unlike a descriptor, push-constant
    // storage is part of the layout itself.
    VkPushConstantRange pushRange {};
    if (pipeline.desc.pushConstantSize > 0) {
        // The RHI does not model per-stage visibility, so every stage of the
        // pipeline's own kind sees the range.
        pushRange.stageFlags = pipeline.compute ? VK_SHADER_STAGE_COMPUTE_BIT
                                                : VK_SHADER_STAGE_ALL_GRAPHICS;
        pushRange.offset = 0;
        pushRange.size = pipeline.desc.pushConstantSize;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
    }
    return vkOk(vkCreatePipelineLayout(device, &layoutInfo, nullptr,
                                       &pipeline.layout),
                "vkCreatePipelineLayout");
}

} // namespace

PipelineHandle VulkanDevice::createPipeline(const PipelineDesc& desc) {
    Impl& d = *impl;
    VulkanShader* shader = d.findShader(desc.shader);
    if (shader == nullptr || shader->vertex == VK_NULL_HANDLE) {
        LOG_ERROR("Vulkan createPipeline: invalid or non-graphics shader");
        return {};
    }
    VulkanPipeline pipeline {};
    pipeline.desc = desc;
    if (!buildLayouts(d.device, d.pcfSampler, *shader, pipeline)) {
        return {};
    }
    // The VkPipeline itself waits for the first setPipeline, when the target's
    // attachment formats are known (PipelineDesc does not carry them).
    const u32 id = d.nextId++;
    d.pipelines.emplace(id, std::move(pipeline));
    return { id };
}

PipelineHandle VulkanDevice::createComputePipeline(
    const ComputePipelineDesc& desc) {
    Impl& d = *impl;
    VulkanShader* shader = d.findShader(desc.shader);
    if (shader == nullptr || shader->compute == VK_NULL_HANDLE) {
        LOG_ERROR("Vulkan createComputePipeline: shader has no compute stage");
        return {};
    }
    VulkanPipeline pipeline {};
    pipeline.compute = true;
    pipeline.desc.shader = desc.shader;
    pipeline.desc.pushConstantSize = desc.pushConstantSize;
    if (!buildLayouts(d.device, d.pcfSampler, *shader, pipeline)) {
        return {};
    }

    VkPipelineShaderStageCreateInfo stage {};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader->compute;
    stage.pName = "main";

    VkComputePipelineCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage = stage;
    info.layout = pipeline.layout;
    if (!vkOk(vkCreateComputePipelines(d.device, d.pipelineCache, 1, &info,
                                       nullptr, &pipeline.computePipeline),
              "vkCreateComputePipelines")) {
        vkDestroyPipelineLayout(d.device, pipeline.layout, nullptr);
        vkDestroyDescriptorSetLayout(d.device, pipeline.setLayout, nullptr);
        return {};
    }

    const u32 id = d.nextId++;
    d.pipelines.emplace(id, std::move(pipeline));
    return { id };
}

void VulkanDevice::destroyPipeline(PipelineHandle handle) {
    Impl& d = *impl;
    auto it = d.pipelines.find(handle.id);
    if (it == d.pipelines.end()) {
        return;
    }
    VulkanDevice::Impl::PendingPipeline pending { {}, it->second.computePipeline,
                              it->second.layout, it->second.setLayout,
                              d.frameCounter };
    pending.variants.reserve(it->second.variants.size());
    for (auto& [key, variant] : it->second.variants) {
        pending.variants.push_back(variant);
    }
    d.pendingPipelines.push_back(std::move(pending));
    d.pipelines.erase(it);
}

// --- GPU markers ------------------------------------------------------------

FenceHandle VulkanDevice::insertFence() {
    Impl& d = *impl;
    VkFenceCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (!vkOk(vkCreateFence(d.device, &info, nullptr, &fence),
              "vkCreateFence(marker)")) {
        return {};
    }
    // An empty submit signals only once every previously submitted command has
    // completed — the GL sync-object semantics the RHI describes.
    if (!vkOk(vkQueueSubmit(d.graphicsQueue, 0, nullptr, fence),
              "vkQueueSubmit(fence marker)")) {
        vkDestroyFence(d.device, fence, nullptr);
        return {};
    }
    const u32 id = d.nextId++;
    d.fences.emplace(id, fence);
    return { id };
}

bool VulkanDevice::fenceReady(FenceHandle handle) {
    Impl& d = *impl;
    auto it = d.fences.find(handle.id);
    if (it == d.fences.end()) {
        return true; // unknown or already consumed
    }
    // Polling only: never block the frame thread (Phase-5 completion queue).
    if (vkGetFenceStatus(d.device, it->second) != VK_SUCCESS) {
        return false;
    }
    vkDestroyFence(d.device, it->second, nullptr); // handles are single-use
    d.fences.erase(it);
    return true;
}

void VulkanDevice::destroyFence(FenceHandle handle) {
    Impl& d = *impl;
    auto it = d.fences.find(handle.id);
    if (it == d.fences.end()) {
        return;
    }
    vkDestroyFence(d.device, it->second, nullptr);
    d.fences.erase(it);
}

TimestampHandle VulkanDevice::insertTimestamp() {
    Impl& d = *impl;
    if (d.queryPool == VK_NULL_HANDLE || !d.frameActive) {
        return {}; // only meaningful while a frame is recording
    }
    // Route to the stream actually being recorded: a scope taken while
    // computeRouting is on brackets compute-queue work.
    const bool onCompute = d.computeRouting && d.computeRecording &&
                           d.computeQueryPool != VK_NULL_HANDLE;
    u32& cursor = onCompute ? d.computeTimestampCursor : d.timestampCursor;
    if (cursor >= kTimestampsPerFrame) {
        LOG_WARN("Vulkan: more than {} timestamps in one frame — dropping",
                 kTimestampsPerFrame);
        return {};
    }
    const u32 query = d.frame * kTimestampsPerFrame + cursor++;
    // BOTTOM_OF_PIPE: the clock is taken when the stream REACHES this point.
    vkCmdWriteTimestamp(onCompute ? d.computeCbs[d.frame]
                                  : d.commandBuffers[d.frame],
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        onCompute ? d.computeQueryPool : d.queryPool, query);
    const u32 id = d.nextId++;
    d.timestamps.emplace(
        id, Impl::PendingTimestamp { query, d.frame,
                                     onCompute
                                         ? d.computeRegionGeneration[d.frame]
                                         : d.regionGeneration[d.frame],
                                     onCompute });
    return { id };
}

bool VulkanDevice::timestampReady(TimestampHandle handle, u64& nanos) {
    Impl& d = *impl;
    auto it = d.timestamps.find(handle.id);
    if (it == d.timestamps.end()) {
        return false;
    }
    const Impl::PendingTimestamp pending = it->second;
    if (pending.cached) { // harvested at slot recycle, before the pool reset
        nanos = static_cast<u64>(static_cast<f64>(pending.cachedTicks) *
                                 d.timestampPeriod);
        d.timestamps.erase(it);
        return true;
    }
    // Its region has been recycled since: the result is gone for good, so
    // release the handle rather than reporting another frame's number.
    const u32 liveGeneration =
        pending.onComputePool ? d.computeRegionGeneration[pending.frameSlot]
                              : d.regionGeneration[pending.frameSlot];
    if (pending.generation != liveGeneration) {
        d.timestamps.erase(it);
        return false;
    }
    u64 ticks = 0;
    const VkResult result = vkGetQueryPoolResults(
        d.device, pending.onComputePool ? d.computeQueryPool : d.queryPool,
        pending.query, 1, sizeof(ticks), &ticks, sizeof(ticks),
        VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS) {
        return false; // VK_NOT_READY — poll again next frame, never block
    }
    nanos = static_cast<u64>(static_cast<f64>(ticks) * d.timestampPeriod);
    d.timestamps.erase(it); // single-use, like fences
    return true;
}

void VulkanDevice::destroyTimestamp(TimestampHandle handle) {
    impl->timestamps.erase(handle.id);
}

// Push descriptors mean there is nothing to allocate here: a bind group is the
// list of writes, replayed against whatever pipeline layout is bound.
BindGroupHandle VulkanDevice::createBindGroup(const BindGroupDesc& desc) {
    if (!validateBindGroupDesc(desc)) {
        return {};
    }
    Impl& d = *impl;
    const u32 id = d.nextId++;
    d.bindGroups.emplace(id, VulkanBindGroup { desc.entries });
    return { id };
}

void VulkanDevice::destroyBindGroup(BindGroupHandle handle) {
    impl->bindGroups.erase(handle.id);
}

} // namespace rhi
