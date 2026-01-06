#include "Buffer.h"
#include <cassert>

namespace graphics {

Buffer::Buffer(VulkanContext* context, size_t allocSize, vk::BufferUsageFlags usage, VmaMemoryUsage memoryUsage)
    : context(context), size(allocSize) {
    
    vk::BufferCreateInfo bufferInfo = {};
    bufferInfo.size = size;
    bufferInfo.usage = usage;

    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = memoryUsage;
    allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer vkBuffer {nullptr};
    const VkResult result = vmaCreateBuffer(context->getAllocator(),
                                      reinterpret_cast<const VkBufferCreateInfo*>(&bufferInfo),
                                      &allocInfo,
                                      &vkBuffer,
                                      &allocation,
                                      nullptr);
    assert(result == VK_SUCCESS && "failed to create buffer!");
    buffer = vk::Buffer(vkBuffer);
}

Buffer::~Buffer() {
    vmaDestroyBuffer(context->getAllocator(), buffer, allocation);
}

void Buffer::map(void** data) {
    vmaMapMemory(context->getAllocator(), allocation, data);
}

void Buffer::unmap() {
    vmaUnmapMemory(context->getAllocator(), allocation);
}

void Buffer::write(void* data, vk::DeviceSize size, vk::DeviceSize offset) {
    void* mappedData;
    map(&mappedData);
    memcpy((char*)mappedData + offset, data, (size_t)size);
    unmap();
}

} // namespace graphics
