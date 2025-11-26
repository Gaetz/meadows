#include "Buffer.h"
#include <cassert>

Buffer::Buffer(VulkanContext* context, vk::DeviceSize size, vk::BufferUsageFlags usage, VmaMemoryUsage memoryUsage)
    : context(context), size(size) {
    
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = (VkBufferUsageFlags)usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = memoryUsage;

    VkBuffer vkBuffer;
    VkResult result = vmaCreateBuffer(context->getAllocator(), &bufferInfo, &allocInfo, &vkBuffer, &allocation, nullptr);
    assert(result == VK_SUCCESS && "failed to create buffer!");
    buffer = vkBuffer;
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
