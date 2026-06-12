#pragma once

#include "../Image.h"

namespace graphics {
    class VulkanContext;
}

namespace graphics::techniques {

    struct GBuffer {
        Image position;
        Image normal;
        Image albedo;
        vk::Extent3D extent;

        void init(VulkanContext* context, vk::Extent3D extent);
        void destroy(VulkanContext* context);
    };

} // namespace graphics::techniques

