//
// Created by dsin on 08/01/2026.
//

#include "DescriptorAllocatorGrowable.h"

namespace graphics {
    DescriptorAllocatorGrowable::DescriptorAllocatorGrowable(vk::Device device, u32 initialSets,  std::span<PoolSizeRatio> poolRatios) :
        device { device } {
        ratios.clear();
        this->ratios = vector<PoolSizeRatio>(poolRatios.begin(), poolRatios.end());

        const vk::DescriptorPool newPool = createPool(setsPerPool, ratios);
        setsPerPool = initialSets * 1.5;
        readyPools.push_back(newPool);
    }

    DescriptorAllocatorGrowable::~DescriptorAllocatorGrowable() {
        for (const auto p : readyPools) {
            device.destroyDescriptorPool(p, nullptr);
        }
        readyPools.clear();
        for (const auto p : fullPools) {
            device.destroyDescriptorPool(p, nullptr);
        }
        fullPools.clear();
    }

    void DescriptorAllocatorGrowable::clear() {
        for (const auto p : readyPools) {
            device.resetDescriptorPool(p, {});
        }
        for (const auto p : fullPools) {
            device.resetDescriptorPool(p, {});
            readyPools.push_back(p);
        }
        fullPools.clear();
    }

    vk::DescriptorSet DescriptorAllocatorGrowable::allocate(const vk::DescriptorSetLayout layout, const void *pNext) {
        // Get or create a pool to allocate from
        vk::DescriptorPool poolToUse = getPool();

        vk::DescriptorSetAllocateInfo allocInfo {};
        allocInfo.pNext = pNext;
        allocInfo.descriptorPool = poolToUse;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &layout;

        vk::DescriptorSet ds;
        const vk::Result result = device.allocateDescriptorSets(&allocInfo, &ds);

        // Allocation failed. Try again
        if (result == vk::Result::eErrorOutOfPoolMemory || result == vk::Result::eErrorFragmentedPool) {
            fullPools.push_back(poolToUse);
            poolToUse = getPool();
            allocInfo.descriptorPool = poolToUse;

            const vk::Result result2 = device.allocateDescriptorSets(&allocInfo, &ds);
            assert(result2 == vk::Result::eSuccess && "Descriptor set allocation failed after creating new pool");
        }

        readyPools.push_back(poolToUse);
        return ds;
    }

    vk::DescriptorPool DescriptorAllocatorGrowable::getPool() {
        vk::DescriptorPool newPool {};
        if (readyPools.size() != 0) {
            newPool = readyPools.back();
            readyPools.pop_back();
        } else {
            // Need to create a new pool
            newPool = createPool(setsPerPool, ratios);

            setsPerPool = setsPerPool * 1.5;
            if (setsPerPool > 4092) {
                setsPerPool = 4092;
            }
        }

        return newPool;
    }

    vk::DescriptorPool DescriptorAllocatorGrowable::createPool(u32 setCount, std::span<PoolSizeRatio> poolRatios) const {
        std::vector<vk::DescriptorPoolSize> poolSizes;
        for (const PoolSizeRatio ratio : poolRatios) {
            vk::DescriptorPoolSize descriptorPoolSize = {};
            descriptorPoolSize.type = ratio.type;
            descriptorPoolSize.descriptorCount = static_cast<u32>(ratio.ratio * setCount);
            poolSizes.push_back(descriptorPoolSize);
        }

        vk::DescriptorPoolCreateInfo poolInfo = {};
        poolInfo.maxSets = setCount;
        poolInfo.poolSizeCount = static_cast<u32>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();

        return device.createDescriptorPool(poolInfo, nullptr);
    }
}