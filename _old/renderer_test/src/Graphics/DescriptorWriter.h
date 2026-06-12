#pragma once
#include <deque>

#include "Types.h"

namespace graphics {

    /**
     * @class DescriptorWriter
     * @brief Helper class for writing/updating resources into Descriptor Sets.
     *
     * ## What does "writing" to a Descriptor Set mean?
     * After allocating a Descriptor Set, it's essentially empty - it doesn't point to any
     * actual resources yet. "Writing" to a descriptor set means binding actual resources
     * (buffers, images, samplers) to the descriptor slots defined by the layout.
     *
     * ## Why use this class?
     * Writing descriptor sets in Vulkan requires:
     * - Creating VkWriteDescriptorSet structures
     * - Creating VkDescriptorImageInfo or VkDescriptorBufferInfo for each resource
     * - Ensuring these info structures stay alive until updateDescriptorSets() is called
     *
     * This class handles all of that complexity by:
     * - Storing image/buffer infos in deques (stable memory addresses)
     * - Batching multiple writes together
     * - Applying all writes in a single updateDescriptorSets() call
     *
     * ## Typical usage:
     * @code
     * DescriptorWriter writer;
     * writer.writeBuffer(0, uniformBuffer, sizeof(UniformData), 0, vk::DescriptorType::eUniformBuffer);
     * writer.writeImage(1, textureView, sampler, vk::ImageLayout::eShaderReadOnlyOptimal,
     *                   vk::DescriptorType::eCombinedImageSampler);
     * writer.updateSet(device, descriptorSet);
     * writer.clear();  // Ready for next set
     * @endcode
     */
    class DescriptorWriter {
    public:
        /**
         * @brief Queues an image/sampler write for a descriptor binding.
         * @param binding The binding index in the descriptor set layout.
         * @param image The image view to bind.
         * @param sampler The sampler to use for sampling the image.
         * @param layout The current layout of the image (usually eShaderReadOnlyOptimal for textures).
         * @param type The descriptor type (eCombinedImageSampler, eSampledImage, etc.).
         */
        void writeImage(int binding, vk::ImageView image, vk::Sampler sampler, vk::ImageLayout layout, vk::DescriptorType type);

        /**
         * @brief Queues a buffer write for a descriptor binding.
         * @param binding The binding index in the descriptor set layout.
         * @param buffer The buffer to bind.
         * @param size The size of the buffer region to bind (in bytes).
         * @param offset The offset into the buffer (in bytes).
         * @param type The descriptor type (eUniformBuffer, eStorageBuffer, etc.).
         */
        void writeBuffer(int binding, vk::Buffer buffer, size_t size, size_t offset, vk::DescriptorType type);

        /**
         * @brief Clears all pending writes, allowing the writer to be reused.
         *
         * Call this after updateSet() to prepare for writing to another descriptor set.
         */
        void clear();

        /**
         * @brief Applies all queued writes to the specified descriptor set.
         * @param device The Vulkan logical device.
         * @param set The descriptor set to update.
         *
         * This calls vkUpdateDescriptorSets() with all the batched writes.
         * After this call, the descriptor set is ready to be used in rendering.
         */
        void updateSet(vk::Device device, vk::DescriptorSet set);

    private:
        /// Stores image descriptor infos. Uses deque for stable addresses (no reallocation invalidation).
        std::deque<vk::DescriptorImageInfo> imageInfos;

        /// Stores buffer descriptor infos. Uses deque for stable addresses.
        std::deque<vk::DescriptorBufferInfo> bufferInfos;

        /// List of pending writes to apply
        vector<vk::WriteDescriptorSet> writes;
    };
}


