//
// Created by dsin on 08/01/2026.
//

#include "DescriptorAllocatorGrowable.h"

namespace graphics {
    DescriptorAllocatorGrowable::DescriptorAllocatorGrowable(vk::Device device, u32 initialSets,  std::span<PoolSizeRatio> poolRatios) :
        device { device } {
        ratios.clear();
        ratios.reserve(poolRatios.size());
        ratios = vector<PoolSizeRatio>(poolRatios.begin(), poolRatios.end());

        setsPerPool = static_cast<u32>(static_cast<float>(initialSets) * 1.5f);
        const vk::DescriptorPool newPool = createPool(setsPerPool, ratios);
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
        if (!readyPools.empty()) {
            newPool = readyPools.back();
            readyPools.pop_back();
        } else {
            // Need to create a new pool
            newPool = createPool(setsPerPool, ratios);

            setsPerPool = static_cast<u32>(static_cast<float>(setsPerPool) * 1.5f);
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
            descriptorPoolSize.descriptorCount = static_cast<u32>(ratio.ratio * static_cast<float>(setCount));
            poolSizes.push_back(descriptorPoolSize);
        }

        vk::DescriptorPoolCreateInfo poolInfo = {};
        poolInfo.maxSets = setCount;
        poolInfo.poolSizeCount = static_cast<u32>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();

        return device.createDescriptorPool(poolInfo, nullptr);
    }

    DescriptorAllocatorGrowable::DescriptorAllocatorGrowable(DescriptorAllocatorGrowable&& other) noexcept
        : device(other.device), setsPerPool(other.setsPerPool), ratios(std::move(other.ratios)),
          fullPools(std::move(other.fullPools)), readyPools(std::move(other.readyPools)) {
        other.device = nullptr;
    }

    DescriptorAllocatorGrowable& DescriptorAllocatorGrowable::operator=(DescriptorAllocatorGrowable&& other) noexcept {
        if (this != &other) {
            // Destroy current pools
            for (const auto p : readyPools) {
                device.destroyDescriptorPool(p, nullptr);
            }
            for (const auto p: fullPools) {
                device.destroyDescriptorPool(p, nullptr);
            }

            device = other.device;
            setsPerPool = other.setsPerPool;
            ratios = std::move(other.ratios);
            fullPools = std::move(other.fullPools);
            readyPools = std::move(other.readyPools);

            other.device = nullptr;
        }
        return *this;
    }
}