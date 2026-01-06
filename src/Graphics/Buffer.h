#pragma once

#include "VulkanContext.h"
#include <vulkan/vulkan.hpp>
#include <vk_mem_alloc.h>

namespace graphics {
    class Buffer {
    public:
        Buffer() = default;
        Buffer(VulkanContext* context, size_t allocSize, vk::BufferUsageFlags usage, VmaMemoryUsage memoryUsage);
        ~Buffer();

        Buffer(const Buffer&) = delete;

        void map(void** data);
        void unmap();
        void write(void* data, vk::DeviceSize size, vk::DeviceSize offset = 0);

        vk::Buffer getBuffer() const { return buffer; }

    public:
        VulkanContext* context {nullptr};
        vk::Buffer buffer {nullptr};
        VmaAllocation allocation;
        VmaAllocationInfo info;
        vk::DeviceSize size {0};
    };


    // Holds the resources needed for a mesh
    struct GPUMeshBuffers {
        Buffer indexBuffer;
        Buffer vertexBuffer;
        vk::DeviceAddress vertexBufferAddress;
    };
} // namespace graphics
