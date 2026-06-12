#include "GBuffer.h"
#include "../VulkanContext.h"

namespace graphics::techniques {

    void GBuffer::init(VulkanContext* context, vk::Extent3D extent) {
        this->extent = extent;

        // Position buffer (RGBA32F for world space position)
        vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;
        position = Image(context, extent, vk::Format::eR32G32B32A32Sfloat, usage);

        // Normal buffer (RGBA16F for world space normals)
        normal = Image(context, extent, vk::Format::eR16G16B16A16Sfloat, usage);

        // Albedo buffer (RGBA8 for color)
        albedo = Image(context, extent, vk::Format::eR8G8B8A8Unorm, usage);
    }

    void GBuffer::destroy(VulkanContext* context) {
        position.destroy(context);
        normal.destroy(context);
        albedo.destroy(context);
    }

} // namespace graphics::techniques

