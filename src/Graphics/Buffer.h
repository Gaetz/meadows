#pragma once

#include "VulkanContext.h"
#include <vulkan/vulkan.hpp>

namespace graphics {
    class Buffer {
    public:
        Buffer() = default;
        Buffer(VulkanContext* context, size_t allocSize, vk::BufferUsageFlags usage, VmaMemoryUsage memoryUsage);
        ~Buffer();

        // Disable copy
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

        // Enable move
        Buffer(Buffer&& other) noexcept;
        Buffer& operator=(Buffer&& other) noexcept;

        void map(void** data) const;
        void unmap() const;
        void write(void* data, vk::DeviceSize size, vk::DeviceSize offset = 0);
        void destroy();

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
