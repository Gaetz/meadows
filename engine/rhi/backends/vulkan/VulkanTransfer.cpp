#include "engine/rhi/backends/vulkan/VulkanInternal.hpp"

#include <algorithm>
#include <cstring>

#include "engine/core/Log.hpp"

namespace rhi {

// --- Transfer helpers ----------------------------------------------------------

bool VulkanDevice::Impl::createStaging(u64 size, bool forRead, VkBuffer& out,
                                       VmaAllocation& outAlloc,
                                       void** outMapped) {
    VkBufferCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = forRead ? VK_BUFFER_USAGE_TRANSFER_DST_BIT
                         : VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // A staging src may be consumed by a copy routed onto the compute
    // queue (async recording) or the upload queue — same CONCURRENT
    // policy as resources.
    const std::array<u32, 3> shareFamilies { graphicsFamily, computeFamily,
                                             transferFamily };
    if (asyncComputeAvailable) {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = uploadQueueAvailable ? 3 : 2;
        info.pQueueFamilyIndices = shareFamilies.data();
    }

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

bool VulkanDevice::Impl::ringAlloc(u64 size, VkBuffer& outBuffer,
                                   u64& outOffset, void** outMapped) {
    StagingRing& ring = stagingRings[frame];
    // 256-byte slices: comfortably above every alignment the copy or the
    // flush could want, and cache-line friendly.
    u64 offset = (ring.head + 255) & ~u64(255);
    if (ring.buffer == VK_NULL_HANDLE || offset + size > ring.capacity) {
        if (ring.buffer != VK_NULL_HANDLE) {
            pendingBuffers.push_back(
                { ring.buffer, ring.allocation, frameCounter });
        }
        u64 capacity = std::max<u64>(ring.capacity * 2, u64(1) << 20);
        while (capacity < size) {
            capacity *= 2;
        }
        ring = {};
        void* mapped = nullptr;
        if (!createStaging(capacity, false, ring.buffer, ring.allocation,
                           &mapped)) {
            ring = {};
            return false;
        }
        ring.mapped = static_cast<u8*>(mapped);
        ring.capacity = capacity;
        offset = 0;
    }
    outBuffer = ring.buffer;
    outOffset = offset;
    *outMapped = ring.mapped + offset;
    ring.head = offset + size;
    return true;
}

bool VulkanDevice::Impl::recordUpload(VkBuffer dst, const void* data,
                                      u64 size, u64 dstOffset) {
    if (!uploadRecording) {
        // Slot reuse is safe: beginFrame waited this slot's last upload
        // value on uploadTimeline.
        VkCommandBufferBeginInfo begin {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!vkOk(vkBeginCommandBuffer(uploadCbs[frame], &begin),
                  "vkBeginCommandBuffer(upload)")) {
            return false;
        }
        uploadRecording = true;
    }
    VkBuffer staging = VK_NULL_HANDLE;
    u64 srcOffset = 0;
    void* mapped = nullptr;
    if (!ringAlloc(size, staging, srcOffset, &mapped)) {
        return false;
    }
    std::memcpy(mapped, data, size);
    vmaFlushAllocation(allocator, stagingRings[frame].allocation, srcOffset,
                       size);
    // WAW within the frame's own upload stream (two updates of the same
    // range); cross-frame WAR and reader visibility ride the semaphores.
    VkBufferMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = dst;
    barrier.offset = dstOffset;
    barrier.size = size;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(uploadCbs[frame], VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                         &barrier, 0, nullptr);
    VkBufferCopy region {};
    region.srcOffset = srcOffset;
    region.dstOffset = dstOffset;
    region.size = size;
    vkCmdCopyBuffer(uploadCbs[frame], staging, dst, 1, &region);
    return true;
}

// Image counterpart of recordUpload: stage through the per-frame ring and
// record transition + copy on the frame's upload command buffer — zero
// submits and zero waits per texture. The endFrame submit chain already
// orders the upload timeline before every reader of the frame.
bool VulkanDevice::Impl::recordImageUpload(
    VkImage image, VkImageAspectFlags aspect, u32 mipLevels, u32 arrayLayers,
    const void* data, u64 size, vector<VkBufferImageCopy> regions) {
    if (!uploadQueueAvailable || !frameActive || computeRouting) {
        return false;
    }
    if (!uploadRecording) {
        VkCommandBufferBeginInfo begin {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!vkOk(vkBeginCommandBuffer(uploadCbs[frame], &begin),
                  "vkBeginCommandBuffer(upload)")) {
            return false;
        }
        uploadRecording = true;
    }
    VkBuffer staging = VK_NULL_HANDLE;
    u64 srcOffset = 0;
    void* mapped = nullptr;
    if (!ringAlloc(size, staging, srcOffset, &mapped)) {
        return false;
    }
    std::memcpy(mapped, data, size);
    vmaFlushAllocation(allocator, stagingRings[frame].allocation, srcOffset,
                       size);
    // Ring offsets are 256-aligned, so the per-mip block alignment the
    // regions were built with still holds after the shift.
    for (VkBufferImageCopy& region : regions) {
        region.bufferOffset += srcOffset;
    }
    VkCommandBuffer cb = uploadCbs[frame];
    transitionLayout(cb, image, aspect, 0, mipLevels, arrayLayers,
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uploadQueueFlags);
    vkCmdCopyBufferToImage(cb, staging, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<u32>(regions.size()), regions.data());
    transitionLayout(cb, image, aspect, 0, mipLevels, arrayLayers,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     uploadQueueFlags);
    return true;
}

} // namespace rhi
