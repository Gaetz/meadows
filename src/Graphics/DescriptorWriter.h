#pragma once
#include <deque>

#include "Types.h"

namespace graphics {
    class DescriptorWriter {
    public:
        void writeImage(int binding, vk::ImageView image, vk::Sampler sampler, vk::ImageLayout layout, vk::DescriptorType type);
        void writeBuffer(int binding, vk::Buffer buffer, size_t size, size_t offset, vk::DescriptorType type);

        void clear();
        void updateSet(vk::Device device, vk::DescriptorSet set);

    private:
        std::deque<vk::DescriptorImageInfo> imageInfos;
        std::deque<vk::DescriptorBufferInfo> bufferInfos;
        vector<vk::WriteDescriptorSet> writes; 
    };
}


