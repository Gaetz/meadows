#pragma once

#include <vulkan/vulkan.hpp>
#include "../Types.h"
#include "../RenderObject.h"

namespace graphics {
    class Renderer;
    class DescriptorAllocatorGrowable;
    struct FrameData;
}

namespace graphics::techniques {

    enum class TechniqueType {
        Basic,
        ShadowMapping,
        Deferred,
        // Add more technique types as needed
    };

    class IRenderingTechnique {
    public:
        virtual ~IRenderingTechnique() = default;

        virtual void init(Renderer* renderer) = 0;
        virtual void cleanup(vk::Device device) = 0;

        virtual void render(
            vk::CommandBuffer cmd,
            const DrawContext& drawContext,
            const GPUSceneData& sceneData,
            DescriptorAllocatorGrowable& frameDescriptors
        ) = 0;

        virtual bool requiresShadowPass() const { return false; }

        virtual const TechniqueType getTechnique() const = 0;
        virtual const str getName() const = 0;

    };

} // namespace graphics::techniques
