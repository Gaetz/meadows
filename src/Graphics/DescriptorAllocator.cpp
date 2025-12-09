#include "DescriptorAllocator.h"

namespace graphics {
    DescriptorAllocator::DescriptorAllocator(const vk::Device device_, const u32 maxSetsPerPool,
        const vector<PoolSizeRatio> &poolRatios) {
        device = device_;

        vector<vk::DescriptorPoolSize> poolSizes;
        for (const auto &[type, ratio] : poolRatios) {
            vk::DescriptorPoolSize poolSize{};
            poolSize.type = type;
            poolSize.descriptorCount = static_cast<u32>(ratio * maxSetsPerPool);
            poolSizes.push_back(poolSize);
        }

        vk::DescriptorPoolCreateInfo poolInfo{};
        poolInfo.maxSets = maxSetsPerPool;
        poolInfo.poolSizeCount = static_cast<u32>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        pool = device.createDescriptorPool(poolInfo);
    }

    DescriptorAllocator::~DescriptorAllocator() {
        device.destroyDescriptorPool(pool);
    }

    vk::DescriptorSet DescriptorAllocator::allocate(const vk::DescriptorSetLayout layout) const {
        vk::DescriptorSetAllocateInfo allocInfo{};
        allocInfo.descriptorPool = pool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &layout;

        const vk::DescriptorSet descriptorSet = device.allocateDescriptorSets(allocInfo).front();
        return descriptorSet;
    }

    void DescriptorAllocator::clearDescriptors() const {
        device.resetDescriptorPool(pool);
    }
}