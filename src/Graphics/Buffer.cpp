/**
 * @file Buffer.cpp
 * @brief Implementation of GPU buffer management with VMA.
 *
 * This file handles buffer creation, memory allocation, data upload,
 * and cleanup for Vulkan buffers using the Vulkan Memory Allocator.
 */

#include "Buffer.h"
#include "VulkanContext.h"
#include <cassert>

namespace graphics {

    // =========================================================================
    // Constructor
    // =========================================================================

    Buffer::Buffer(VulkanContext* context, size_t allocSize, vk::BufferUsageFlags usage, VmaMemoryUsage memoryUsage)
        : context(context), buffer(nullptr), allocation(), info(), size(allocSize) {
        allocation = nullptr;
        info = {};

        // Set up buffer creation info
        vk::BufferCreateInfo bufferInfo = {};
        bufferInfo.size = size;
        bufferInfo.usage = usage;

        // Set up VMA allocation info
        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = memoryUsage;

        // Automatically map CPU-visible buffers for convenience
        // This allows direct access via info.pMappedData without explicit map/unmap
        if (memoryUsage == VMA_MEMORY_USAGE_CPU_ONLY || memoryUsage == VMA_MEMORY_USAGE_CPU_TO_GPU) {
            allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        }

        // Create the buffer and allocate memory in one call
        VkBuffer vkBuffer {nullptr};
        const VkResult result = vmaCreateBuffer(context->getAllocator(),
                                      reinterpret_cast<const VkBufferCreateInfo*>(&bufferInfo),
                                      &allocInfo,
                                      &vkBuffer,
                                      &allocation,
                                      &info);  // info contains pMappedData if mapped
        assert(result == VK_SUCCESS && "failed to create buffer!");
        buffer = vk::Buffer(vkBuffer);
    }

    // =========================================================================
    // Destructor
    // =========================================================================

    Buffer::~Buffer() {
        destroy();
    }

    // =========================================================================
    // Move Constructor
    // =========================================================================

    Buffer::Buffer(Buffer&& other) noexcept
        : context(other.context)
        , buffer(other.buffer)
        , allocation(other.allocation)
        , info(other.info)
        , size(other.size) {
        // Invalidate the moved-from object to prevent double-destruction
        other.context = nullptr;
        other.buffer = nullptr;
        other.allocation = nullptr;
        other.size = 0;
    }

    // =========================================================================
    // Move Assignment
    // =========================================================================

    Buffer& Buffer::operator=(Buffer&& other) noexcept {
        if (this != &other) {
            // Destroy current resources before taking new ones
            destroy();

            // Transfer ownership
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

    // =========================================================================
    // Memory Mapping
    // =========================================================================

    void Buffer::map(void** data) const {
        // Map the buffer memory so CPU can access it
        // Only works for CPU-visible memory types
        vmaMapMemory(context->getAllocator(), allocation, data);
    }

    void Buffer::unmap() const {
        // Unmap the buffer memory
        // For persistently mapped buffers (CPU_TO_GPU), this is not strictly necessary
        vmaUnmapMemory(context->getAllocator(), allocation);
    }

    // =========================================================================
    // Data Writing
    // =========================================================================

    void Buffer::write(void* data, vk::DeviceSize size, vk::DeviceSize offset) {
        // Map, copy, unmap - simple but not optimal for frequent updates
        // For frequently updated buffers, keep memory mapped and write directly
        void* mappedData;
        map(&mappedData);
        memcpy((char*)mappedData + offset, data, (size_t)size);
        unmap();
    }

    // =========================================================================
    // Cleanup
    // =========================================================================

    void Buffer::destroy() {
        if (context && buffer) {
            // VMA handles both buffer destruction and memory deallocation
            vmaDestroyBuffer(context->getAllocator(), buffer, allocation);
            buffer = nullptr;
            allocation = nullptr;
            context = nullptr;
        }
    }
} // namespace graphics
