#pragma once
#include "Types.h"
#include "VulkanLoader.h"

namespace graphics {
    struct RenderObject {
        u32 indexCount;
        u32 firstIndex;
        vk::Buffer indexBuffer;

        MaterialInstance* material;
        Bounds bounds;

        Mat4 transform;
        vk::DeviceAddress vertexBufferAddress;
    };

    struct DrawContext {
        vector<RenderObject> opaqueSurfaces;
        std::vector<RenderObject> transparentSurfaces;
    };
}
