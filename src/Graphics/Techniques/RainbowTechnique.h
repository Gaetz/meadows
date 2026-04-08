#pragma once

#include "IRenderingTechnique.h"
#include "../Buffer.h"
#include "../DescriptorAllocatorGrowable.h"
#include "../MaterialPipeline.h"

namespace graphics::techniques {

    class RainbowTechnique : public IRenderingTechnique {
    public:
        // ── User-facing parameters (edited via ImGui) ────────────────────────
        struct Params {
            float sunElevation       = 30.0f;  // degrees above horizon
            float dropletRadius      = 0.5f;   // mm
            float refractiveIndex    = 1.333f;
            float primaryIntensity   = 3.0f;
            float secondaryIntensity = 0.8f;
            bool  showPrimary        = true;
            bool  showSecondary      = true;
            int   numWavelengths     = 24;
            float altitude           = 0.f;    // metres (0 – 12000)
            float intensityMult      = 2.f;    // multiplicateur global des arcs
        } params;

        // ── IRenderingTechnique ──────────────────────────────────────────────
        void init(Renderer* renderer) override;
        void cleanup(vk::Device device) override;
        void render(vk::CommandBuffer cmd,
                    const DrawContext& drawContext,
                    const GPUSceneData& sceneData,
                    DescriptorAllocatorGrowable& frameDescriptors) override;

        const TechniqueType getTechnique() const override { return TechniqueType::Rainbow; }
        const str getName() const override { return "Rainbow"; }

    private:
        // GPU-side uniform buffer layout (matches rainbow_sky.frag)
        struct RainbowUBO {
            glm::mat4 invViewProj;
            glm::vec4 cameraPos;
            glm::vec4 sunDir;
            float     dropletRadius;
            float     nBase;
            float     primaryIntensity;
            float     secondaryIntensity;
            int       showPrimary;
            int       showSecondary;
            int       numWavelengths;
            float     altitude;   // remplace _pad, même offset (124)
        };

        void createDescriptors();
        void createPipeline();

        Renderer* renderer { nullptr };

        Buffer                      uboBuffer;
        vk::DescriptorSetLayout     descriptorLayout { nullptr };
        vk::DescriptorSet           descriptorSet    { nullptr };
        DescriptorAllocatorGrowable descriptorPool;

        vk::PipelineLayout        pipelineLayout { nullptr };
        uptr<MaterialPipeline>    pipeline;
    };

} // namespace graphics::techniques
