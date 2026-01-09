#pragma once
#include "Types.h"

namespace graphics {

    class VulkanContext;
    class ImmediateSubmitter;

    class Image {
    public:
        Image() = default;
        Image(VulkanContext* context, vk::Extent3D size, vk::Format format, vk::ImageUsageFlags usage, bool mipmapped = false);
        Image(VulkanContext* context, ImmediateSubmitter& submitter, void* data, vk::Extent3D size, vk::Format format, vk::ImageUsageFlags usage, bool mipmapped = false);
        ~Image() = default;

        void destroy(const VulkanContext* context);

        VulkanContext* context { nullptr };
        vk::Image image;
        VmaAllocation allocation;
        vk::ImageView imageView;
        vk::Extent3D imageExtent;
        vk::Format imageFormat;

        Image(Image&& other) noexcept;
        Image& operator=(Image&& other) noexcept;

        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;
    };
}