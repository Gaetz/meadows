/**
 * @file DescriptorWriter.cpp
 * @brief Implementation of the Descriptor Set writer helper.
 *
 * This file implements a helper class that simplifies writing resources
 * (images, buffers) to Vulkan Descriptor Sets.
 */

#include "DescriptorWriter.h"

namespace graphics {

    // =========================================================================
    // Image Writing
    // =========================================================================

    void DescriptorWriter::writeImage(const int binding, const vk::ImageView image, const vk::Sampler sampler,
                                                const vk::ImageLayout layout, const vk::DescriptorType type) {
        // Create the image descriptor info
        // This describes which image and sampler to use
        vk::DescriptorImageInfo descriptorImageInfo{};
        descriptorImageInfo.sampler = sampler;      // Sampler for texture filtering
        descriptorImageInfo.imageView = image;      // View into the image
        descriptorImageInfo.imageLayout = layout;   // Current layout of the image

        // Store in deque - deque guarantees stable addresses even when growing,
        // which is crucial because WriteDescriptorSet stores a pointer to this info
        const vk::DescriptorImageInfo &info = imageInfos.emplace_back(descriptorImageInfo);

        // Create the write descriptor
        vk::WriteDescriptorSet write{};
        write.dstBinding = binding;         // Target binding in the descriptor set
        write.dstSet = nullptr;             // Will be set in updateSet()
        write.descriptorCount = 1;          // Writing one descriptor
        write.descriptorType = type;        // Type must match the layout
        write.pImageInfo = &info;           // Pointer to our stable image info

        writes.push_back(write);
    }

    // =========================================================================
    // Buffer Writing
    // =========================================================================

    void DescriptorWriter::writeBuffer(const int binding, const vk::Buffer buffer,
                                                 const size_t size, const size_t offset, const vk::DescriptorType type) {
        // Create the buffer descriptor info
        // This describes which buffer region to bind
        vk::DescriptorBufferInfo descriptorBufferInfo{};
        descriptorBufferInfo.buffer = buffer;   // The buffer handle
        descriptorBufferInfo.offset = offset;   // Offset into the buffer
        descriptorBufferInfo.range = size;      // Size of the region to bind

        // Store in deque for stable addresses
        const vk::DescriptorBufferInfo &info = bufferInfos.emplace_back(descriptorBufferInfo);

        // Create the write descriptor
        vk::WriteDescriptorSet write{};
        write.dstBinding = binding;         // Target binding in the descriptor set
        write.dstSet = nullptr;             // Will be set in updateSet()
        write.descriptorCount = 1;          // Writing one descriptor
        write.descriptorType = type;        // Type must match the layout
        write.pBufferInfo = &info;          // Pointer to our stable buffer info

        writes.push_back(write);
    }

    // =========================================================================
    // Utility Functions
    // =========================================================================

    void DescriptorWriter::clear() {
        // Clear all stored infos and writes
        // This prepares the writer for reuse with a new descriptor set
        imageInfos.clear();
        writes.clear();
        bufferInfos.clear();
    }

    void DescriptorWriter::updateSet(vk::Device device, vk::DescriptorSet set) {
        // Set the destination descriptor set for all pending writes
        for (auto &write : writes) {
            write.dstSet = set;
        }

        // Apply all writes in a single Vulkan call
        // This is more efficient than calling updateDescriptorSets() for each write
        device.updateDescriptorSets(static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
    }
}

