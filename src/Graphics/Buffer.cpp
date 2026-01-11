#include "Buffer.h"
#include "VulkanContext.h"
#include <cassert>

namespace graphics {

Buffer::Buffer(VulkanContext* context, size_t allocSize, vk::BufferUsageFlags usage, VmaMemoryUsage memoryUsage)
    : context(context), buffer(nullptr), allocation(), info(), size(allocSize) {
    allocation = nullptr;
    info = {};

    vk::BufferCreateInfo bufferInfo = {};
    bufferInfo.size = size;
    bufferInfo.usage = usage;

    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = memoryUsage;

    // Only map CPU-visible buffers
    if (memoryUsage == VMA_MEMORY_USAGE_CPU_ONLY || memoryUsage == VMA_MEMORY_USAGE_CPU_TO_GPU) {
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    VkBuffer vkBuffer {nullptr};
    const VkResult result = vmaCreateBuffer(context->getAllocator(),
                                      reinterpret_cast<const VkBufferCreateInfo*>(&bufferInfo),
                                      &allocInfo,
                                      &vkBuffer,
                                      &allocation,
                                      &info);
    assert(result == VK_SUCCESS && "failed to create buffer!");
    buffer = vk::Buffer(vkBuffer);
}

Buffer::~Buffer() {
    destroy();
}

Buffer::Buffer(Buffer&& other) noexcept
    : context(other.context)
    , buffer(other.buffer)
    , allocation(other.allocation)
    , info(other.info)
    , size(other.size) {
    // Invalidate the moved-from object
    other.context = nullptr;
    other.buffer = nullptr;
    other.allocation = nullptr;
    other.size = 0;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        // Destroy current resources
        destroy();

        // Move from other
        context = other.context;
        buffer = other.buffer;
        allocation = other.allocation;
        info = other.info;
        size = other.size;

        // Invalidate the moved-from object
        other.context = nullptr;
        other.buffer = nullptr;
        other.allocation = nullptr;
        other.size = 0;
    }
    return *this;
}

void Buffer::map(void** data) const {
    vmaMapMemory(context->getAllocator(), allocation, data);
}

void Buffer::unmap() const {
    vmaUnmapMemory(context->getAllocator(), allocation);
}

void Buffer::write(void* data, vk::DeviceSize size, vk::DeviceSize offset) {
    void* mappedData;
    map(&mappedData);
    memcpy((char*)mappedData + offset, data, (size_t)size);
    unmap();
}

void Buffer::destroy() {
    if (context && buffer) {
        vmaDestroyBuffer(context->getAllocator(), buffer, allocation);
        buffer = nullptr;
        allocation = nullptr;
        context = nullptr;
    }
}
} // namespace graphics
