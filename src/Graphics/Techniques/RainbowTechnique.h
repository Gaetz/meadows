#pragma once

#include "IRenderingTechnique.h"
#include "../Buffer.h"
#include "../DescriptorAllocatorGrowable.h"
#include "../MaterialPipeline.h"
#include <chrono>

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

            // Terrain
            bool  showTerrain        = true;
            int   terrainGridSize    = 64;     // patches par côté
            float terrainPatchSize   = 16.f;   // taille monde d'un patch
            float terrainHeight      = 150.f;  // amplitude verticale
            float terrainFrequency   = 0.008f;
            float terrainPersistence = 0.5f;
            int   terrainOctaves     = 7;
            float terrainAmbient     = 0.15f;
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

        // GPU-side uniform buffer for terrain (matches terrain.mesh / terrain.frag)
        struct TerrainUBO {
            glm::mat4 viewProj;        // offset   0
            glm::vec4 cameraPos;       // offset  64
            glm::vec4 sunDir;          // offset  80
            float     patchSize;       // offset  96
            float     heightScale;     // offset 100
            float     fbmFrequency;    // offset 104
            float     fbmPersistence;  // offset 108
            int       fbmOctaves;      // offset 112
            int       gridSize;        // offset 116
            float     ambientLight;    // offset 120
            float     time;            // offset 124
        };  // 128 bytes

        void createDescriptors();
        void createPipeline();
        void createTerrainDescriptors();
        void createTerrainPipeline();

        Renderer* renderer { nullptr };

        Buffer                      uboBuffer;
        vk::DescriptorSetLayout     descriptorLayout { nullptr };
        vk::DescriptorSet           descriptorSet    { nullptr };
        DescriptorAllocatorGrowable descriptorPool;

        vk::PipelineLayout        pipelineLayout { nullptr };
        uptr<MaterialPipeline>    pipeline;

        Buffer                      terrainUboBuffer;
        vk::DescriptorSetLayout     terrainDescriptorLayout { nullptr };
        vk::DescriptorSet           terrainDescriptorSet    { nullptr };
        DescriptorAllocatorGrowable terrainDescriptorPool;
        vk::PipelineLayout          terrainPipelineLayout   { nullptr };
        vk::Pipeline                terrainPipeline         { nullptr };
        PFN_vkCmdDrawMeshTasksEXT   pfnDrawMeshTasksEXT     { nullptr };

        float accumTime { 0.0f };
        std::chrono::steady_clock::time_point lastFrameTime {};
    };

} // namespace graphics::techniques
