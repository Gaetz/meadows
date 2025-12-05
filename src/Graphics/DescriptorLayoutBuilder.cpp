#include "DescriptorLayoutBuilder.hpp"
#include "DescriptorLayoutBuilder.hpp"

namespace graphics
{
    void DescriptorLayoutBuilder::addBinding(u32 binding, vk::DescriptorType type) {
        vk::DescriptorSetLayoutBinding newBinding {};
        newBinding.binding = binding;
        newBinding.descriptorType = type;
        newBinding.descriptorCount = 1;

        bindings.push_back(newBinding);
    }

    void DescriptorLayoutBuilder::clear() {
        bindings.clear();
    }

    vk::DescriptorSetLayout DescriptorLayoutBuilder::build(vk::Device device, vk::ShaderStageFlags shaderStages,
        void* pNext, vk::DescriptorSetLayoutCreateFlagBits flags) {
        for (auto& binding : bindings) {
            binding.stageFlags |= shaderStages;
        }

        vk::DescriptorSetLayoutCreateInfo info {};
        info.pNext = pNext;
        info.bindingCount = (u32)bindings.size();
        info.pBindings = bindings.data();
        info.flags = flags;

        vk::DescriptorSetLayout set;
        set = device.createDescriptorSetLayout(info);
        return set;
        return set;
    }
}
