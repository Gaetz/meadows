//
// Created by dsin on 08/01/2026.
//

#include "DescriptorWriter.h"

namespace graphics {
    void DescriptorWriter::writeImage(const int binding, const vk::ImageView image, const vk::Sampler sampler,
                                                const vk::ImageLayout layout, const vk::DescriptorType type) {
        vk::DescriptorImageInfo descriptorImageInfo{};
        descriptorImageInfo.sampler = sampler;
        descriptorImageInfo.imageView = image;
        descriptorImageInfo.imageLayout = layout;
        const vk::DescriptorImageInfo &info = imageInfos.emplace_back(descriptorImageInfo);

        vk::WriteDescriptorSet write{};
        write.dstBinding = binding;
        write.dstSet = nullptr; // Left empty for now until we need to write it
        write.descriptorCount = 1;
        write.descriptorType = type;
        write.pImageInfo = &info;

        writes.push_back(write);
    }

    void DescriptorWriter::writeBuffer(const int binding, const vk::Buffer buffer,
                                                 const size_t size, const size_t offset, const vk::DescriptorType type) {
        vk::DescriptorBufferInfo descriptorBufferInfo{};
        descriptorBufferInfo.buffer = buffer;
        descriptorBufferInfo.offset = offset;
        descriptorBufferInfo.range = size;
        const vk::DescriptorBufferInfo &info = bufferInfos.emplace_back(descriptorBufferInfo);

        vk::WriteDescriptorSet write{};
        write.dstBinding = binding;
        write.dstSet = nullptr; // Left empty for now until we need to write it
        write.descriptorCount = 1;
        write.descriptorType = type;
        write.pBufferInfo = &info;

        writes.push_back(write);
    }

    void DescriptorWriter::clear() {
        imageInfos.clear();
        writes.clear();
        bufferInfos.clear();
    }

    void DescriptorWriter::updateSet(vk::Device device, vk::DescriptorSet set) {
        for (auto &write : writes) {
            write.dstSet = set;
        }
        device.updateDescriptorSets(static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
    }
}

