/**
 * @file Renderer.h
 * @brief Main rendering orchestrator for Vulkan-based graphics.
 *
 * This file defines the Renderer class which coordinates the entire rendering
 * pipeline, from acquiring swapchain images to presenting the final frame.
 */

#pragma once

#include "../Defines.h"
#include "VulkanContext.h"
#include <vulkan/vulkan.hpp>
#include <vector>
#include <chrono>

#include "Buffer.h"
#include "Camera.h"
#include "ComputeEffect.h"
#include "DeletionQueue.hpp"
#include "DescriptorAllocatorGrowable.h"
#include "MaterialPipeline.h"
#include "RenderObject.h"
#include "Utils.hpp"
#include "VulkanLoader.h"
#include "Pipelines/GLTFMetallicRoughness.h"
#include "Pipelines/ShadowPipeline.h"
#include "ShadowMap.h"
#include "Techniques/BloomTechnique.h"
#include "Techniques/SSAOTechnique.h"

class Scene;

namespace graphics::techniques {
    class IRenderingTechnique;
}

namespace graphics {
    struct Node;

    /**
     * @struct FrameData
     * @brief Per-frame resources for double/triple buffering.
     *
     * ## What is Frame Overlap?
     * While the GPU renders frame N, the CPU prepares frame N+1.
     * This requires separate resources for each "in-flight" frame to avoid conflicts.
     *
     * ## What's in FrameData:
     * - **Command Pool/Buffer**: Records GPU commands for this frame
     * - **Semaphore**: Signals when swapchain image is ready
     * - **Fence**: CPU waits on this before reusing frame resources
     * - **Deletion Queue**: Deferred cleanup for this frame's temporary resources
     * - **Frame Descriptors**: Per-frame descriptor set allocator
     */
    struct FrameData {
        vk::CommandPool commandPool;              ///< Pool for allocating command buffers
        vk::CommandBuffer mainCommandBuffer;      ///< The command buffer for this frame
        vk::Semaphore imageAvailableSemaphore;    ///< Signaled when swapchain image is acquired
        vk::Fence renderFence;                    ///< Signaled when GPU finishes this frame
        DeletionQueue deletionQueue;              ///< Deferred deletion for frame resources
        DescriptorAllocatorGrowable frameDescriptors; ///< Per-frame descriptor allocator
    };

    /**
     * @class Renderer
     * @brief Orchestrates the entire Vulkan rendering pipeline.
     *
     * ## The Renderer's Role
     * The Renderer is the central hub that:
     * 1. Manages frame synchronization (CPU/GPU coordination)
     * 2. Handles swapchain image acquisition and presentation
     * 3. Delegates scene rendering to pluggable techniques
     * 4. Applies post-processing effects (bloom, SSAO)
     * 5. Renders the ImGui debug interface
     *
     * ## The Frame Rendering Process (draw() method)
     *
     * ### Step 1: Wait for Previous Frame
     * ```cpp
     * device.waitForFences(renderFence);  // CPU waits for GPU
     * ```
     * Before reusing frame resources, we must ensure the GPU is done with them.
     *
     * ### Step 2: Acquire Swapchain Image
     * ```cpp
     * device.acquireNextImageKHR(swapchain, ..., imageAvailableSemaphore);
     * ```
     * Requests the next image from the swapchain. The semaphore signals when ready.
     *
     * ### Step 3: Record Commands
     * ```cpp
     * commandBuffer.begin();
     * // ... all rendering commands ...
     * commandBuffer.end();
     * ```
     * Fill the command buffer with all GPU work for this frame.
     *
     * ### Step 4: Submit to GPU
     * ```cpp
     * queue.submit(commandBuffer, waitSemaphore, signalSemaphore, fence);
     * ```
     * - Wait on imageAvailableSemaphore before starting
     * - Signal renderFinishedSemaphore when done
     * - Signal fence so CPU knows when frame is complete
     *
     * ### Step 5: Present
     * ```cpp
     * queue.presentKHR(swapchain, imageIndex, waitSemaphore);
     * ```
     * Display the rendered image. Waits on renderFinishedSemaphore.
     *
     * ## Rendering Flow Diagram
     * ```
     * [Acquire Image] -> [Render Scene] -> [Post-Process] -> [Copy to Swapchain] -> [ImGui] -> [Present]
     *        |                  |                |                    |               |
     *   Semaphore          Technique         Bloom/SSAO           Blit            Overlay
     * ```
     *
     * ## Pluggable Rendering Techniques
     * The Renderer delegates actual scene rendering to IRenderingTechnique implementations:
     * - BasicTechnique: Simple forward rendering
     * - ShadowMappingTechnique: Forward + shadow maps
     * - DeferredRenderingTechnique: G-Buffer based deferred
     */
    class Renderer {
    public:
        Renderer(VulkanContext* context);
        ~Renderer();

        /// Initializes all rendering resources (called once at startup)
        void init();

        /// Cleans up all Vulkan resources
        void cleanup();

        /**
         * @brief Renders a complete frame.
         *
         * This is the main entry point called every frame. It orchestrates:
         * 1. Frame synchronization
         * 2. Scene rendering (via technique)
         * 3. Post-processing
         * 4. Swapchain presentation
         */
        void draw();

        /// Forwards SDL events to the camera
        void processEvent(const SDL_Event& event);

        /// Uploads mesh data to GPU buffers
        GPUMeshBuffers uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices);

        // =====================================================================
        // Accessors
        // =====================================================================
        VulkanContext* getContext() const { return context; }
        vk::DescriptorSetLayout getSceneDataDescriptorLayout() const { return gpuSceneDataDescriptorLayout; }
        vk::DescriptorSetLayout getShadowSceneDataDescriptorLayout() const { return shadowSceneDataDescriptorLayout; }
        ImmediateSubmitter* getImmediateSubmitter() { return &immSubmitter; }
        ShadowMap* getShadowMap() { return shadowMap.get(); }
        Buffer& getSceneDataBuffer() { return sceneDataBuffer; }
        const Buffer& getSceneDataBuffer() const { return sceneDataBuffer; }

        // =====================================================================
        // External Scene Integration
        // =====================================================================

        /// Sets an external draw context (from Scene)
        void setDrawContext(DrawContext* ctx) { externalDrawContext = ctx; }
        DrawContext* getDrawContext() { return externalDrawContext ? externalDrawContext : &mainDrawContext; }

        /// Sets an external rendering technique (from Scene)
        void setRenderingTechnique(techniques::IRenderingTechnique* technique) { externalRenderingTechnique = technique; }
        techniques::IRenderingTechnique* getRenderingTechnique() const { return externalRenderingTechnique; }

        DescriptorAllocatorGrowable& getCurrentFrameDescriptors() { return getCurrentFrame().frameDescriptors; }
        GPUSceneData& getSceneData() { return sceneData; }

        /// Sets the active scene for ImGui rendering
        void setActiveScene(::Scene* scene) { activeScene = scene; }
        ::Scene* getActiveScene() const { return activeScene; }

        // =====================================================================
        // Rendering Configuration
        // =====================================================================

        void setAnimateLight(bool animate) { animateLight = animate; }
        bool isAnimatingLight() const { return animateLight; }

        Image& getSceneImage() { return sceneImage; }
        techniques::BloomParams& getBloomParams() { return bloom.getParams(); }
        const techniques::BloomParams& getBloomParams() const { return bloom.getParams(); }
        techniques::SSAOParams& getSSAOParams() { return ssao.getParams(); }
        const techniques::SSAOParams& getSSAOParams() const { return ssao.getParams(); }

        // =====================================================================
        // Default Resources (available for materials)
        // =====================================================================
        Image errorCheckerboardImage;   ///< Magenta/black checkerboard for missing textures
        Image whiteImage;               ///< 1x1 white pixel
        Image blackImage;               ///< 1x1 black pixel
        Image greyImage;                ///< 1x1 grey pixel
        vk::Sampler defaultSamplerLinear;   ///< Bilinear filtering sampler
        vk::Sampler defaultSamplerNearest;  ///< Nearest-neighbor sampler
        pipelines::GLTFMetallicRoughness metalRoughMaterial;  ///< Default PBR material
        Camera mainCamera;              ///< Main camera for the scene

    private:
        // =====================================================================
        // Initialization Methods
        // =====================================================================
        void createCommandPoolAndBuffers();
        void createSyncObjects();
        void createDescriptors();
        void createPipelines();
        void createBackgroundPipeline();
        void createTrianglePipeline();
        void createMeshPipeline();
        void createSceneData();
        void createPostProcessResources();

        // =====================================================================
        // Rendering Methods
        // =====================================================================
        void initImGui();
        void drawImGui(vk::CommandBuffer commandBuffer);
        void drawBackground(vk::CommandBuffer, const vk::DescriptorSet* targetDescriptors = nullptr, const Image* targetImage = nullptr);
        void drawGeometry(vk::CommandBuffer);
        void drawShadowPass(vk::CommandBuffer);
        void drawShadowGeometry(vk::CommandBuffer, vk::DescriptorSet sceneDescriptor);
        void drawShadowDebug(vk::CommandBuffer, vk::DescriptorSet sceneDescriptor);
        void updateLightMatrices();
        void updateScene();
        void applyPostProcess(vk::CommandBuffer cmd);

        float getMinRenderScale() const;

        // =====================================================================
        // Core State
        // =====================================================================
        VulkanContext* context;         ///< Vulkan context (device, queues, etc.)
        bool resizeRequested {false};   ///< Flag for swapchain recreation
        float renderScale { 0.5f };     ///< Resolution scale factor
        int frameNumber {0};            ///< Current frame counter

        // =====================================================================
        // Frame Synchronization
        // =====================================================================
        /**
         * FRAME_OVERLAP determines how many frames can be "in flight" simultaneously.
         * With 2, CPU can prepare frame N+1 while GPU renders frame N.
         * This hides latency and improves throughput.
         */
        static const int FRAME_OVERLAP = 2;
        FrameData frames[FRAME_OVERLAP];
        FrameData& getCurrentFrame() { return frames[frameNumber % FRAME_OVERLAP]; };

        /// One semaphore per swapchain image for proper synchronization
        std::vector<vk::Semaphore> renderFinishedSemaphores;

        // =====================================================================
        // Descriptor Sets
        // =====================================================================
        vk::DescriptorSetLayout drawImageDescriptorLayout;
        vk::DescriptorSet drawImageDescriptors;
        vk::DescriptorSet sceneImageDescriptors;  // For background compute on sceneImage

        // =====================================================================
        // Immediate Submission (for uploads)
        // =====================================================================
        ImmediateSubmitter immSubmitter;

        // =====================================================================
        // Compute Effects (background rendering)
        // =====================================================================
        vk::PipelineLayout computePipelineLayout;
        std::vector<ComputeEffect> backgroundEffects;
        int currentBackgroundEffect{0};

        // =====================================================================
        // Graphics Pipelines
        // =====================================================================
        uptr<MaterialPipeline> trianglePipeline;
        uptr<MaterialPipeline> meshPipeline;

        // =====================================================================
        // Scene Data
        // =====================================================================
        GPUMeshBuffers rectangleMesh;
        vector<sptr<MeshAsset>> testMeshes;
        GPUSceneData sceneData;                           ///< Camera matrices, lighting
        Buffer sceneDataBuffer;                           ///< GPU buffer for scene data
        vk::DescriptorSetLayout gpuSceneDataDescriptorLayout;
        vk::DescriptorSetLayout shadowSceneDataDescriptorLayout;
        vk::DescriptorSetLayout singleImageDescriptorLayout;

        // =====================================================================
        // Shadow Mapping
        // =====================================================================
        uptr<ShadowMap> shadowMap;
        pipelines::ShadowPipeline shadowPipeline;
        bool displayShadowMap { false };
        bool enablePCF { true };
        bool animateLight { true };
        Vec3 lightPos { 40.0f, 50.0f, 25.0f };  // Above scene (Y inverted from VulkanDemo)
        float lightFOV { 45.0f };
        float lightAngle { 0.0f };

        // =====================================================================
        // Materials
        // =====================================================================
        MaterialInstance defaultData;
        Buffer defaultMaterialConstants;

        // =====================================================================
        // Draw Context
        // =====================================================================
        DrawContext mainDrawContext;
        std::unordered_map<std::string, sptr<Node>> loadedNodes;
        std::unordered_map<std::string, std::shared_ptr<LoadedGLTF>> loadedScenes;
        DrawContext* externalDrawContext { nullptr };

        // =====================================================================
        // Rendering Technique
        // =====================================================================
        techniques::IRenderingTechnique* externalRenderingTechnique { nullptr };
        ::Scene* activeScene { nullptr };

        // =====================================================================
        // Optimization State Caching
        // =====================================================================
        MaterialPipeline* lastPipeline { nullptr };
        MaterialInstance* lastMaterial { nullptr };
        vk::Buffer lastIndexBuffer { nullptr };

        // =====================================================================
        // ImGui
        // =====================================================================
        vk::DescriptorPool imguiDescriptorPool;
        float rotationSpeed = 1.0f;
        float accumulatedRotation = 0.0f;
        std::chrono::high_resolution_clock::time_point lastFrameTime;

        // =====================================================================
        // Post-Processing
        // =====================================================================
        Image sceneImage;           ///< Intermediate render target before post-processing
        Image ssaoOutputImage;      ///< Output after SSAO (before bloom)
        techniques::BloomTechnique bloom;
        techniques::SSAOTechnique ssao;
    };

} // namespace graphics
