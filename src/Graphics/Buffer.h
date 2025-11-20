#pragma once

#include "VulkanContext.h"
#include <vulkan/vulkan.hpp>

class Buffer {
public:
    Buffer(VulkanContext* context, vk::DeviceSize size, vk::BufferUsageFlags usage, VmaMemoryUsage memoryUsage);
    ~Buffer();

    void map(void** data);
    void unmap();
    void write(void* data, vk::DeviceSize size, vk::DeviceSize offset = 0);

    vk::Buffer getBuffer() const { return buffer; }

private:
    VulkanContext* context;
    vk::Buffer buffer;
    VmaAllocation allocation;
    vk::DeviceSize size;
};
