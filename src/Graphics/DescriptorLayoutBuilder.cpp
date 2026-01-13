/**
 * @file DescriptorLayoutBuilder.cpp
 * @brief Implementation of the Descriptor Set Layout builder.
 *
 * This file implements a builder pattern for creating Vulkan Descriptor Set Layouts,
 * which define the structure of descriptor sets used to bind resources to shaders.
 */

#include "DescriptorLayoutBuilder.hpp"

namespace graphics
{
    void DescriptorLayoutBuilder::addBinding(u32 binding, vk::DescriptorType type) {
        // Create a new binding configuration
        vk::DescriptorSetLayoutBinding newBinding {};
        newBinding.binding = binding;           // Binding index (matches shader layout(binding = X))
        newBinding.descriptorType = type;       // Type of resource at this binding
        newBinding.descriptorCount = 1;         // Single descriptor (not an array)

        bindings.push_back(newBinding);
    }

    void DescriptorLayoutBuilder::clear() {
        bindings.clear();
    }

    vk::DescriptorSetLayout DescriptorLayoutBuilder::build(vk::Device device, vk::ShaderStageFlags shaderStages,
        void* pNext, vk::DescriptorSetLayoutCreateFlagBits flags) {

        // Apply the shader stage flags to all bindings
        // This tells Vulkan which shader stages can access each binding
        for (auto& binding : bindings) {
            binding.stageFlags |= shaderStages;
        }

        // Create the layout info structure
        vk::DescriptorSetLayoutCreateInfo info {};
        info.pNext = pNext;                                     // Optional extensions
        info.bindingCount = static_cast<u32>(bindings.size());  // Number of bindings
        info.pBindings = bindings.data();                       // Array of binding descriptions
        info.flags = flags;                                     // Optional creation flags

        // Create and return the descriptor set layout
        vk::DescriptorSetLayout set;
        set = device.createDescriptorSetLayout(info);
        return set;
    }
}
