#pragma once

#include "Types.h"
namespace graphics {
    struct PoolSizeRatio{
        vk::DescriptorType type;
        float ratio;
    };

    class DescriptorAllocator {
    public:
        DescriptorAllocator(const vk::Device device, const u32 maxSetsPerPool, const vector<PoolSizeRatio>& poolRatios);
        ~DescriptorAllocator();

        vk::DescriptorSet allocate(vk::DescriptorSetLayout layout) const;
        void clearDescriptors() const;
    private:
        vk::Device device;
        vk::DescriptorPool pool;
    };
}