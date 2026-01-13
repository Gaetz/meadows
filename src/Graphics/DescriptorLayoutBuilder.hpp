#pragma once
#include <vulkan/vulkan.hpp>

#include "DescriptorLayoutBuilder.hpp"
#include "Types.h"

namespace graphics
{
    /**
     * @class DescriptorLayoutBuilder
     * @brief Builder pattern for creating Vulkan Descriptor Set Layouts.
     *
     * ## What is a Descriptor Set Layout?
     * A Descriptor Set Layout defines the structure of a Descriptor Set: which bindings exist,
     * what type of resource each binding holds (uniform buffer, sampler, etc.), and at which
     * shader stages they are accessible.
     *
     * Think of it as a "blueprint" that tells Vulkan: "My descriptor set will have a uniform
     * buffer at binding 0 and a texture sampler at binding 1."
     *
     * ## Why use a builder?
     * Creating a Descriptor Set Layout requires filling multiple Vulkan structures.
     * This builder simplifies the process by:
     * - Allowing you to add bindings one by one with addBinding()
     * - Handling all the boilerplate structure setup in build()
     * - Making the code more readable and less error-prone
     *
     * ## Typical usage:
     * @code
     * DescriptorLayoutBuilder builder;
     * builder.addBinding(0, vk::DescriptorType::eUniformBuffer);
     * builder.addBinding(1, vk::DescriptorType::eCombinedImageSampler);
     * vk::DescriptorSetLayout layout = builder.build(device, vk::ShaderStageFlagBits::eFragment);
     * @endcode
     */
    class DescriptorLayoutBuilder
    {
    public:
        /// List of bindings that will be included in the layout
        vector<vk::DescriptorSetLayoutBinding> bindings;

        /**
         * @brief Adds a new binding to the layout.
         * @param binding The binding index (must match the binding number in your shader).
         * @param type The type of descriptor (eUniformBuffer, eCombinedImageSampler, etc.).
         *
         * Each binding represents one resource slot in the descriptor set.
         * The descriptor count is set to 1 (single resource, not an array).
         */
        void addBinding(u32 binding, vk::DescriptorType type);

        /**
         * @brief Clears all bindings, allowing the builder to be reused.
         */
        void clear();

        /**
         * @brief Builds and returns the final Descriptor Set Layout.
         * @param device The Vulkan logical device.
         * @param shaderStages Which shader stages can access these descriptors
         *                     (e.g., eVertex, eFragment, or combined with |).
         * @param pNext Optional pointer for Vulkan extensions (pNext chain).
         * @param flags Optional creation flags for the layout.
         * @return The created Descriptor Set Layout.
         *
         * @note The returned layout must be destroyed when no longer needed
         *       using device.destroyDescriptorSetLayout().
         */
        vk::DescriptorSetLayout build(vk::Device device, vk::ShaderStageFlags shaderStages, void* pNext = nullptr, vk::DescriptorSetLayoutCreateFlagBits flags = {});
    };
}