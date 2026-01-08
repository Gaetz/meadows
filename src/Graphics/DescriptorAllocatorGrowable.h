#pragma once
#include "Types.h"

namespace graphics {
    class DescriptorAllocatorGrowable {
    public:

        struct PoolSizeRatio {
            vk::DescriptorType type;
            float ratio;
        };
        DescriptorAllocatorGrowable() = default;
        DescriptorAllocatorGrowable(vk::Device device, u32 initialSets, std::span<PoolSizeRatio> poolRatios);
        ~DescriptorAllocatorGrowable();

        void clear();
        vk::DescriptorSet allocate(vk::DescriptorSetLayout layout, const void* pNext = nullptr);

        private:
        vk::DescriptorPool getPool();
        vk::DescriptorPool createPool(u32 setCount, std::span<PoolSizeRatio> poolRatios) const;

        vk::Device device { nullptr };
        u32 setsPerPool { 0 };
        vector<PoolSizeRatio> ratios;
        vector<vk::DescriptorPool> fullPools;
        vector<vk::DescriptorPool> readyPools;

    };
}
