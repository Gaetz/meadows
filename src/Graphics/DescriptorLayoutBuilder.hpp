#pragma once
#include <vulkan/vulkan.hpp>

#include "DescriptorLayoutBuilder.hpp"
#include "Types.h"

namespace graphics
{
    class DescriptorLayoutBuilder
    {
    public:
        vector<vk::DescriptorSetLayoutBinding> bindings;

        void addBinding(u32 binding, vk::DescriptorType type);
        void clear();
        vk::DescriptorSetLayout build(vk::Device device, vk::ShaderStageFlags shaderStages, void* pNext = nullptr, vk::DescriptorSetLayoutCreateFlagBits flags = {});
    };
}