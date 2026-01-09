#pragma once
#include "Types.h"

namespace graphics {
    class VulkanContext;
    class Image {
    public:
        Image() = default;
        Image(VulkanContext* context, vk::Extent3D size, vk::Format format, vk::ImageUsageFlags usage, bool mipmapped = false);
        Image(VulkanContext* context, void* data, vk::Extent3D size, vk::Format format, vk::ImageUsageFlags usage, bool mipmapped = false);
        ~Image() = default;

        void destroy(const VulkanContext* context);

        VulkanContext* context { nullptr };
        vk::Image image;
        VmaAllocation allocation;
        vk::ImageView imageView;
        vk::Extent3D imageExtent;
        vk::Format imageFormat;
    };
}