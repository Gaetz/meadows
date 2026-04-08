#include "RainbowTechnique.h"
#include "../Renderer.h"
#include "../VulkanContext.h"
#include "../PipelineBuilder.h"
#include "../DescriptorLayoutBuilder.hpp"
#include "../DescriptorWriter.h"
#include "../Utils.hpp"
#include "BasicServices/Log.h"
#include "BasicServices/File.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/transform.hpp>

using services::Log;

namespace graphics::techniques {

    void RainbowTechnique::init(Renderer* renderer) {
        this->renderer = renderer;
        VulkanContext* ctx = renderer->getContext();
        vk::Device device = ctx->getDevice();

        // UBO buffer (CPU-visible, written every frame)
        uboBuffer = Buffer(ctx,
            sizeof(RainbowUBO),
            vk::BufferUsageFlagBits::eUniformBuffer,
            VMA_MEMORY_USAGE_CPU_TO_GPU);

        createDescriptors();
        createPipeline();
        createTerrainDescriptors();
        createTerrainPipeline();

        Log::Info("RainbowTechnique initialized");
    }

    void RainbowTechnique::cleanup(vk::Device device) {
        uboBuffer.destroy();

        pipeline.reset();
        if (pipelineLayout)       { device.destroyPipelineLayout(pipelineLayout);          pipelineLayout = nullptr; }
        if (descriptorLayout)     { device.destroyDescriptorSetLayout(descriptorLayout);   descriptorLayout = nullptr; }

        terrainUboBuffer.destroy();
        if (terrainPipeline)        { device.destroyPipeline(terrainPipeline);                terrainPipeline = nullptr; }
        if (terrainPipelineLayout)  { device.destroyPipelineLayout(terrainPipelineLayout);    terrainPipelineLayout = nullptr; }
        if (terrainDescriptorLayout){ device.destroyDescriptorSetLayout(terrainDescriptorLayout); terrainDescriptorLayout = nullptr; }
    }

    void RainbowTechnique::createDescriptors() {
        vk::Device device = renderer->getContext()->getDevice();

        DescriptorLayoutBuilder builder;
        builder.addBinding(0, vk::DescriptorType::eUniformBuffer);
        descriptorLayout = builder.build(device, vk::ShaderStageFlagBits::eFragment);

        vector<DescriptorAllocatorGrowable::PoolSizeRatio> sizes = {
            { vk::DescriptorType::eUniformBuffer, 1 }
        };
        descriptorPool = DescriptorAllocatorGrowable(device, 1, sizes);
        descriptorSet  = descriptorPool.allocate(descriptorLayout);

        // Write the UBO binding once (buffer doesn't change, only its contents)
        DescriptorWriter writer;
        writer.writeBuffer(0, uboBuffer.buffer, sizeof(RainbowUBO), 0,
                           vk::DescriptorType::eUniformBuffer);
        writer.updateSet(device, descriptorSet);
    }

    void RainbowTechnique::createPipeline() {
        vk::Device device = renderer->getContext()->getDevice();

        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts    = &descriptorLayout;
        pipelineLayout = device.createPipelineLayout(layoutInfo);

        vk::Format colorFormat = renderer->getSceneImage().imageFormat;

        PipelineBuilder builder(renderer->getContext(),
                                "shaders/rainbow_sky.vert.spv",
                                "shaders/rainbow_sky.frag.spv");
        builder.pipelineLayout = pipelineLayout;
        builder.setColorAttachmentFormat(colorFormat);
        builder.setDepthFormat(vk::Format::eD32Sfloat);
        builder.disableDepthTest();
        builder.disableBlending();
        builder.setMultisamplingNone();
        builder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise);
        pipeline = builder.buildPipeline(device);
    }

    void RainbowTechnique::createTerrainDescriptors() {
        vk::Device device = renderer->getContext()->getDevice();

        terrainUboBuffer = Buffer(renderer->getContext(),
            sizeof(TerrainUBO),
            vk::BufferUsageFlagBits::eUniformBuffer,
            VMA_MEMORY_USAGE_CPU_TO_GPU);

        DescriptorLayoutBuilder builder;
        builder.addBinding(0, vk::DescriptorType::eUniformBuffer);
        terrainDescriptorLayout = builder.build(device,
            vk::ShaderStageFlagBits::eMeshEXT | vk::ShaderStageFlagBits::eFragment);

        vector<DescriptorAllocatorGrowable::PoolSizeRatio> sizes = {
            { vk::DescriptorType::eUniformBuffer, 1 }
        };
        terrainDescriptorPool = DescriptorAllocatorGrowable(device, 1, sizes);
        terrainDescriptorSet  = terrainDescriptorPool.allocate(terrainDescriptorLayout);

        DescriptorWriter writer;
        writer.writeBuffer(0, terrainUboBuffer.buffer, sizeof(TerrainUBO), 0,
                           vk::DescriptorType::eUniformBuffer);
        writer.updateSet(device, terrainDescriptorSet);
    }

    void RainbowTechnique::createTerrainPipeline() {
        vk::Device device = renderer->getContext()->getDevice();

        // Pipeline layout
        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts    = &terrainDescriptorLayout;
        terrainPipelineLayout = device.createPipelineLayout(layoutInfo);

        // Shader modules
        vk::ShaderModule meshMod = graphics::createShaderModule(
            services::File::readBinary("shaders/terrain.mesh.spv"), device);
        vk::ShaderModule fragMod = graphics::createShaderModule(
            services::File::readBinary("shaders/terrain.frag.spv"), device);

        // Shader stages: mesh + fragment (no vertex, no input assembly)
        std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
        stages[0] = vk::PipelineShaderStageCreateInfo{{}, vk::ShaderStageFlagBits::eMeshEXT,  meshMod, "main"};
        stages[1] = vk::PipelineShaderStageCreateInfo{{}, vk::ShaderStageFlagBits::eFragment, fragMod, "main"};

        // Dynamic rendering format info
        vk::Format colorFmt = renderer->getSceneImage().imageFormat;
        vk::Format depthFmt = vk::Format::eD32Sfloat;

        vk::PipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.colorAttachmentCount    = 1;
        renderingInfo.pColorAttachmentFormats = &colorFmt;
        renderingInfo.depthAttachmentFormat   = depthFmt;

        // Rasterization: fill, no culling (terrain viewed from all angles)
        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode    = vk::CullModeFlagBits::eNone;
        raster.frontFace   = vk::FrontFace::eCounterClockwise;
        raster.lineWidth   = 1.0f;

        // Depth test enabled, write enabled
        vk::PipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.depthTestEnable  = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp   = vk::CompareOp::eLessOrEqual;

        // No multisampling
        vk::PipelineMultisampleStateCreateInfo multisampling{};
        multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;

        // No blending — terrain is fully opaque
        vk::PipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask =
            vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        blendAttachment.blendEnable = VK_FALSE;

        vk::PipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments    = &blendAttachment;

        // Dynamic viewport and scissor
        vk::PipelineViewportStateCreateInfo viewportState{};
        viewportState.viewportCount = 1;
        viewportState.scissorCount  = 1;

        std::array<vk::DynamicState, 2> dynamicStates = {
            vk::DynamicState::eViewport, vk::DynamicState::eScissor
        };
        vk::PipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates    = dynamicStates.data();

        // No pVertexInputState, no pInputAssemblyState — mesh shaders don't use them
        vk::GraphicsPipelineCreateInfo info{};
        info.pNext               = &renderingInfo;
        info.stageCount          = static_cast<uint32_t>(stages.size());
        info.pStages             = stages.data();
        info.pRasterizationState = &raster;
        info.pMultisampleState   = &multisampling;
        info.pDepthStencilState  = &depthStencil;
        info.pColorBlendState    = &colorBlend;
        info.pViewportState      = &viewportState;
        info.pDynamicState       = &dynamicState;
        info.layout              = terrainPipelineLayout;

        terrainPipeline = device.createGraphicsPipeline(nullptr, info).value;

        device.destroyShaderModule(meshMod);
        device.destroyShaderModule(fragMod);

        // Load extension function pointer (not in static loader)
        pfnDrawMeshTasksEXT = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(
            vkGetDeviceProcAddr(static_cast<VkDevice>(device), "vkCmdDrawMeshTasksEXT"));
    }

    void RainbowTechnique::render(vk::CommandBuffer cmd,
                                  const DrawContext& /*drawContext*/,
                                  const GPUSceneData& sceneData,
                                  DescriptorAllocatorGrowable& /*frameDescriptors*/) {
        // ── Update UBO ──────────────────────────────────────────────────────
        RainbowUBO ubo{};
        ubo.invViewProj = glm::inverse(sceneData.viewProj);

        // Camera position from inverse view matrix
        glm::mat4 invView = glm::inverse(sceneData.view);
        ubo.cameraPos = invView[3]; // Last column = world position

        // Sun direction: FROM observer TOWARD sun
        float elevRad = glm::radians(params.sunElevation);
        ubo.sunDir = glm::vec4(0.0f, glm::sin(elevRad), glm::cos(elevRad), 0.0f);

        ubo.dropletRadius      = params.dropletRadius;
        ubo.nBase              = params.refractiveIndex;
        ubo.primaryIntensity   = params.primaryIntensity   * params.intensityMult;
        ubo.secondaryIntensity = params.secondaryIntensity * params.intensityMult;
        ubo.showPrimary    = params.showPrimary   ? 1 : 0;
        ubo.showSecondary  = params.showSecondary ? 1 : 0;
        ubo.numWavelengths = params.numWavelengths;
        ubo.altitude       = params.altitude;

        memcpy(uboBuffer.info.pMappedData, &ubo, sizeof(RainbowUBO));

        // ── Render sky + terrain in one beginRendering/endRendering ─────────
        Image& sceneImage = renderer->getSceneImage();
        Image& depthImage = renderer->getContext()->getDepthImage();

        // Transition depth image to eDepthAttachmentOptimal
        graphics::transitionImage(cmd, depthImage.image,
            vk::ImageLayout::eUndefined,
            vk::ImageLayout::eDepthAttachmentOptimal,
            vk::ImageAspectFlagBits::eDepth);

        vk::RenderingAttachmentInfo colorAttachment{};
        colorAttachment.imageView   = sceneImage.imageView;
        colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        colorAttachment.loadOp      = vk::AttachmentLoadOp::eClear;
        colorAttachment.storeOp     = vk::AttachmentStoreOp::eStore;
        colorAttachment.clearValue  = vk::ClearColorValue{0.0f, 0.0f, 0.0f, 1.0f};

        vk::RenderingAttachmentInfo depthAttachment{};
        depthAttachment.imageView   = depthImage.imageView;
        depthAttachment.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
        depthAttachment.loadOp      = vk::AttachmentLoadOp::eClear;
        depthAttachment.storeOp     = vk::AttachmentStoreOp::eStore;
        depthAttachment.clearValue  = vk::ClearDepthStencilValue{1.0f, 0};

        auto extent = renderer->getContext()->getDrawImage().imageExtent;
        vk::RenderingInfo renderInfo{};
        renderInfo.renderArea           = vk::Rect2D{{0, 0}, {extent.width, extent.height}};
        renderInfo.layerCount           = 1;
        renderInfo.colorAttachmentCount = 1;
        renderInfo.pColorAttachments    = &colorAttachment;
        renderInfo.pDepthAttachment     = &depthAttachment;

        vk::Viewport viewport{};
        viewport.x        = 0;
        viewport.y        = 0;
        viewport.width    = static_cast<float>(extent.width);
        viewport.height   = static_cast<float>(extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        vk::Rect2D scissor{{0, 0}, {extent.width, extent.height}};

        cmd.beginRendering(renderInfo);

        // ── Pass 1 : sky (fullscreen triangle, depth disabled) ───────────────
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->getPipeline());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout,
                               0, 1, &descriptorSet, 0, nullptr);
        cmd.setViewport(0, viewport);
        cmd.setScissor(0, scissor);
        cmd.draw(3, 1, 0, 0); // Fullscreen triangle — no vertex buffer needed

        // ── Pass 2 : terrain (mesh shader, depth enabled) ────────────────────
        if (params.showTerrain && terrainPipeline) {
            // Update terrain UBO
            glm::mat4 terrainInvView = glm::inverse(sceneData.view);
            float elevRad = glm::radians(params.sunElevation);

            TerrainUBO tubo{};
            tubo.viewProj       = sceneData.viewProj;
            tubo.cameraPos      = terrainInvView[3];
            tubo.sunDir         = glm::vec4(0.0f, glm::sin(elevRad), glm::cos(elevRad), 0.0f);
            tubo.patchSize      = params.terrainPatchSize;
            tubo.heightScale    = params.terrainHeight;
            tubo.fbmFrequency   = params.terrainFrequency;
            tubo.fbmPersistence = params.terrainPersistence;
            tubo.fbmOctaves     = params.terrainOctaves;
            tubo.gridSize       = params.terrainGridSize;
            tubo.ambientLight   = params.terrainAmbient;
            memcpy(terrainUboBuffer.info.pMappedData, &tubo, sizeof(TerrainUBO));

            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, terrainPipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                   terrainPipelineLayout, 0, 1,
                                   &terrainDescriptorSet, 0, nullptr);
            cmd.setViewport(0, viewport);
            cmd.setScissor(0, scissor);

            int g = params.terrainGridSize;
            if (pfnDrawMeshTasksEXT) {
                pfnDrawMeshTasksEXT(static_cast<VkCommandBuffer>(cmd),
                                    static_cast<uint32_t>(g),
                                    static_cast<uint32_t>(g), 1);
            }
        }

        cmd.endRendering();
    }

} // namespace graphics::techniques
