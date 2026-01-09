#include "Renderer.h"

#include <vk_mem_alloc.h>
#include <chrono>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>
#include <glm/gtx/transform.hpp>

#include "Swapchain.h"
#include "MaterialPipeline.h"
#include "Buffer.h"
#include "DescriptorLayoutBuilder.hpp"
#include "DescriptorWriter.h"
#include "Image.h"
#include "PipelineBuilder.h"
#include "Utils.hpp"
#include "VulkanInit.hpp"
#include "fmt/color.h"

using graphics::pipelines::GLTFMetallicRoughness;

namespace graphics {

    Renderer::Renderer(VulkanContext *context) : context(context) {
        lastFrameTime = std::chrono::high_resolution_clock::now();
    }

    Renderer::~Renderer() {
        cleanup();
    }

    void Renderer::init() {
        createCommandPoolAndBuffers();
        createSyncObjects();
        createDescriptors();
        createPipelines();
        createSceneData();
        initImGui();
    }

    void Renderer::cleanup() {
        if (!frames[0].commandPool) return; // Already cleaned up or never initialized

        vk::Device device = context->getDevice();
        device.waitIdle();

        // Cleanup ImGui
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();

        device.destroyDescriptorPool(imguiDescriptorPool);

        for (int i = 0; i < FRAME_OVERLAP; i++) {
            frames[i].deletionQueue.flush();

            device.destroyCommandPool(frames[i].commandPool);
            device.destroySemaphore(frames[i].imageAvailableSemaphore);
            device.destroyFence(frames[i].renderFence);
        }

        for (auto &semaphore: renderFinishedSemaphores) {
            device.destroySemaphore(semaphore);
        }
        renderFinishedSemaphores.clear();

        context->flushMainDeletionQueue();
    }

    void Renderer::createCommandPoolAndBuffers() {
        const QueueFamilyIndices queueFamilyIndices = context->findQueueFamilies(context->getPhysicalDevice());

        const auto poolInfo = graphics::commandPoolCreateInfo(
            queueFamilyIndices.graphicsFamily.value(),
            vk::CommandPoolCreateFlagBits::eResetCommandBuffer
        );

        // Frame-specific command pools and buffers
        for (int i = 0; i < FRAME_OVERLAP; i++) {
            frames[i].commandPool = context->getDevice().createCommandPool(poolInfo);

            vk::CommandBufferAllocateInfo cmdAllocInfo = graphics::commandBufferAllocateInfo(frames[i].commandPool, 1);
            frames[i].mainCommandBuffer = context->getDevice().allocateCommandBuffers(cmdAllocInfo)[0];
        }

        // Immediate command pool
        immSubmitter.immCommandPool = context->getDevice().createCommandPool(poolInfo);
        vk::CommandBufferAllocateInfo cmdAllocInfo = graphics::commandBufferAllocateInfo(immSubmitter.immCommandPool, 1);
        immSubmitter.immCommandBuffer = context->getDevice().allocateCommandBuffers(cmdAllocInfo)[0];
        context->addToMainDeletionQueue([this]() {
            context->getDevice().destroyCommandPool(immSubmitter.immCommandPool);
        }, "Immediate command pool");
    }

    void Renderer::createDescriptors() {
        vk::Device device = context->getDevice();

        // Make the descriptor set layout for our compute draw
        DescriptorLayoutBuilder layoutBuilder;
        layoutBuilder.addBinding(0, vk::DescriptorType::eStorageImage);
        drawImageDescriptorLayout = layoutBuilder.build(device, vk::ShaderStageFlagBits::eCompute);

        // Allocate a descriptor set for our draw image
        drawImageDescriptors = context->getGlobalDescriptorAllocator()->allocate(drawImageDescriptorLayout);

        // Write the descriptor set to point to our draw image
        DescriptorWriter writer;
        writer.writeImage(0, context->getDrawImage().imageView, nullptr, vk::ImageLayout::eGeneral, vk::DescriptorType::eStorageImage);

        writer.updateSet(device, drawImageDescriptors);

        // Cleanup
        context->addToMainDeletionQueue([&]() {
            context->getDevice().destroyDescriptorSetLayout(drawImageDescriptorLayout);
        }, "drawImageDescriptorLayout");

        // Create per-frame descriptor allocators
        for (int i = 0; i < FRAME_OVERLAP; i++) {
            // create a descriptor pool
            vector<DescriptorAllocatorGrowable::PoolSizeRatio> frameSizes {
                { vk::DescriptorType::eStorageImage, 3 },
                { vk::DescriptorType::eStorageBuffer, 3 },
                { vk::DescriptorType::eUniformBuffer, 3 },
                { vk::DescriptorType::eCombinedImageSampler, 4 }
            };

            frames[i].frameDescriptors = DescriptorAllocatorGrowable { device, 1000, frameSizes };
        }

        // We use uniform buffer here instead of SSBO because this is a small buffer.
        // We aren't using it through  buffer device address because we have a single
        // descriptor set for all objects so there isn't any overhead of managing it.
        DescriptorLayoutBuilder builder;
        builder.addBinding(0, vk::DescriptorType::eUniformBuffer);
        gpuSceneDataDescriptorLayout = builder.build(device, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment);

        // Simple texture descriptor
        DescriptorLayoutBuilder builderTexture;
        builderTexture.addBinding(0, vk::DescriptorType::eCombinedImageSampler);
        singleImageDescriptorLayout = builderTexture.build(device, vk::ShaderStageFlagBits::eFragment);
    }

    void Renderer::createPipelines() {
        createBackgroundPipeline();
        //createTrianglePipeline();
        createMeshPipeline();
        metalRoughMaterial.buildPipelines(this);
    }

    void Renderer::createBackgroundPipeline() {
        vk::PipelineLayoutCreateInfo computeLayout{};
        computeLayout.setLayoutCount = 1;
        computeLayout.pSetLayouts = &drawImageDescriptorLayout;

        /*
        // Basic compute pipeline layout
        pipelineLayout = context->getDevice().createPipelineLayout(computeLayout);
        computePipeline = std::make_unique<PipelineCompute>(context, "shaders/gradient.comp.spv", pipelineLayout);
        */


        // Pipeline layout with push constants
        vk::PushConstantRange pushConstant{};
        pushConstant.offset = 0;
        pushConstant.size = sizeof(ComputePushConstants);
        pushConstant.stageFlags = vk::ShaderStageFlagBits::eCompute;

        computeLayout.pushConstantRangeCount = 1;
        computeLayout.pPushConstantRanges = &pushConstant;
        computePipelineLayout = context->getDevice().createPipelineLayout(computeLayout);

        /*
        // Simple compute pipeline with push constants
        computePipeline = std::make_unique<PipelineCompute>(context, "shaders/gradientCustom.comp.spv", pipelineLayout);
        */

        ComputeEffect gradient{"Gradient", context, "shaders/gradientCustom.comp.spv", computePipelineLayout};
        gradient.data.data1 = Vec4{1, 0, 0, 1};
        gradient.data.data2 = Vec4{0, 0, 1, 1};

        ComputeEffect sky{"Sky", context, "shaders/sky.comp.spv", computePipelineLayout};
        sky.data.data1 = Vec4{0.1, 0.2, 0.4, 0.97};

        context->addToMainDeletionQueue([this]() { context->getDevice().destroyPipelineLayout(computePipelineLayout); },
                                        "Renderer's pipelineLayout");

        backgroundEffects.push_back(gradient);
        backgroundEffects.push_back(sky);
    }

    void Renderer::createTrianglePipeline() {
        const vk::Device device = context->getDevice();

        vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
        const vk::PipelineLayout trianglePipelineLayout = device.createPipelineLayout(pipelineLayoutInfo);

        PipelineBuilder pipelineBuilder { context, "shaders/coloredTriangle.vert.spv", "shaders/coloredTriangle.frag.spv"};
        pipelineBuilder.pipelineLayout = trianglePipelineLayout;
        pipelineBuilder.setInputTopology(vk::PrimitiveTopology::eTriangleList);
        pipelineBuilder.setPolygonMode(vk::PolygonMode::eFill);
        pipelineBuilder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eClockwise);
        pipelineBuilder.setMultisamplingNone();
        pipelineBuilder.disableBlending();
        pipelineBuilder.disableDepthTest();

        // Connect the image format we will draw into, from draw image
        pipelineBuilder.setColorAttachmentFormat(context->getDrawImage().imageFormat);
        pipelineBuilder.setDepthFormat(context->getDepthImage().imageFormat);

        trianglePipeline = pipelineBuilder.buildPipeline(device);
    }

    void Renderer::createMeshPipeline() {
        const vk::Device device = context->getDevice();

        vk::PushConstantRange bufferRange {};
        bufferRange.offset = 0;
        bufferRange.size = sizeof(GraphicsPushConstants);
        bufferRange.stageFlags = vk::ShaderStageFlagBits::eVertex;

        vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.pPushConstantRanges = &bufferRange;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        /* Simple mesh pipeline
        const vk::PipelineLayout meshPipelineLayout = device.createPipelineLayout(pipelineLayoutInfo);

        PipelineBuilder pipelineBuilder { context, "shaders/coloredTriangleMesh.vert.spv", "shaders/coloredTriangle.frag.spv"};
        */

        // Textured mesh pipeline
        pipelineLayoutInfo.pSetLayouts = &singleImageDescriptorLayout;
        pipelineLayoutInfo.setLayoutCount = 1;
        const vk::PipelineLayout meshPipelineLayout = device.createPipelineLayout(pipelineLayoutInfo);

        PipelineBuilder pipelineBuilder { context, "shaders/coloredTriangleMesh.vert.spv", "shaders/texImage.frag.spv"};
        pipelineBuilder.pipelineLayout = meshPipelineLayout;
        pipelineBuilder.setInputTopology(vk::PrimitiveTopology::eTriangleList);
        pipelineBuilder.setPolygonMode(vk::PolygonMode::eFill);
        pipelineBuilder.setCullMode(vk::CullModeFlagBits::eNone, vk::FrontFace::eClockwise);
        pipelineBuilder.setMultisamplingNone();
        pipelineBuilder.enableBlendingAlphaBlend();
        pipelineBuilder.enableDepthTest(true, vk::CompareOp::eGreaterOrEqual);

        // Connect the image format we will draw into, from draw image
        pipelineBuilder.setColorAttachmentFormat(context->getDrawImage().imageFormat);
        pipelineBuilder.setDepthFormat(context->getDepthImage().imageFormat);

        meshPipeline = pipelineBuilder.buildPipeline(device);
    }

    void Renderer::createSyncObjects() {
        vk::SemaphoreCreateInfo semaphoreInfo = graphics::semaphoreCreateInfo();
        vk::FenceCreateInfo fenceInfo = graphics::fenceCreateInfo(vk::FenceCreateFlagBits::eSignaled);

        vk::Device device = context->getDevice();

        // Create command pool sync objects for frames in flight
        for (size_t i = 0; i < FRAME_OVERLAP; i++) {
            frames[i].renderFence = device.createFence(fenceInfo);
            frames[i].imageAvailableSemaphore = device.createSemaphore(semaphoreInfo);
        }

        // Create one render finished semaphore per swapchain image
        size_t imageCount = context->getSwapchain()->getImages().size();
        renderFinishedSemaphores.resize(imageCount);
        for (size_t i = 0; i < imageCount; i++) {
            renderFinishedSemaphores[i] = device.createSemaphore(semaphoreInfo);
        }

        // Create a fence for immediate submits
        immSubmitter.immFence = device.createFence(graphics::fenceCreateInfo());
        context->addToMainDeletionQueue([this]() {
            context->getDevice().destroyFence(immSubmitter.immFence);
        }, "immFence");
    }

    void Renderer::drawBackground(vk::CommandBuffer command) {
        // Clear color
        /*
        float flash = std::abs(std::sin(frameNumber / 120.f));
        vk::ClearColorValue clearValue = vk::ClearColorValue { 0.0f, 0.0f, flash, 1.0f };

        vk::ImageSubresourceRange clearRange = graphics::imageSubresourceRange(vk::ImageAspectFlagBits::eColor);
        command.clearColorImage(context->getDrawImage().image,
            vk::ImageLayout::eGeneral, &clearValue,1, &clearRange);
        */


        // Bind compute pipeline and descriptor sets
        constexpr auto bindPoint = vk::PipelineBindPoint::eCompute;
        const ComputeEffect &effect = backgroundEffects[currentBackgroundEffect];
        command.bindPipeline(bindPoint, effect.getPipeline());
        command.bindDescriptorSets(bindPoint, effect.getPipelineLayout(), 0, 1, &drawImageDescriptors, 0, nullptr);
        command.pushConstants<ComputePushConstants>(effect.getPipelineLayout(), vk::ShaderStageFlagBits::eCompute, 0,
                                                    effect.data);

        command.dispatch(std::ceil(context->getDrawImage().imageExtent.width / 16.0f),
                         std::ceil(context->getDrawImage().imageExtent.height / 16.0f),
                         1);
    }

    void Renderer::drawGeometry(vk::CommandBuffer command) {
        // Begin a render pass connected to our draw image
        vk::RenderingAttachmentInfo colorAttachment = graphics::attachmentInfo(context->getDrawImage().imageView, nullptr, vk::ImageLayout::eColorAttachmentOptimal);
        vk::RenderingAttachmentInfo depthAttachment = graphics::depthAttachmentInfo(context->getDepthImage().imageView, vk::ImageLayout::eDepthAttachmentOptimal);

        const auto imageExtent = context->getDrawImage().imageExtent;
        // Fix: initialize vk::Rect2D with an explicit offset and the extent to avoid narrowing conversions
        const vk::Rect2D rect{ vk::Offset2D{0, 0}, {imageExtent.width, imageExtent.height} };
        const vk::RenderingInfo renderInfo = graphics::renderingInfo(rect, &colorAttachment, &depthAttachment);
        command.beginRendering(&renderInfo);



        // Allocate a new uniform buffer for the scene data
        Buffer gpuSceneDataBuffer { context, sizeof(GPUSceneData), vk::BufferUsageFlagBits::eUniformBuffer, VMA_MEMORY_USAGE_CPU_TO_GPU };

        // Add it to the deletion queue of this frame so it gets deleted once its been used
        /*
        getCurrentFrame().deletionQueue.pushFunction([&gpuSceneDataBuffer]() {
            gpuSceneDataBuffer.destroy();
        }, "Frame GPU scene data buffer");
        */

        // Write the buffer
        auto sceneUniformData = static_cast<GPUSceneData *>(gpuSceneDataBuffer.info.pMappedData);
        *sceneUniformData = sceneData;

        // Create a descriptor set that binds that buffer and update it
        VkDescriptorSet globalDescriptor = getCurrentFrame().frameDescriptors.allocate(gpuSceneDataDescriptorLayout);

        {
            DescriptorWriter writer;
            writer.writeBuffer(0, gpuSceneDataBuffer.buffer, sizeof(GPUSceneData), 0, vk::DescriptorType::eUniformBuffer);
            writer.updateSet(context->getDevice(), globalDescriptor);
        }



        // Draw triangle
        /*
        trianglePipeline->bind(command);

        vk::Viewport viewport = {};
        viewport.x = 0;
        viewport.y = 0;
        viewport.width = imageExtent.width;
        viewport.height = imageExtent.height;
        viewport.minDepth = 0.f;
        viewport.maxDepth = 1.f;
        command.setViewport(0, 1, &viewport);

        vk::Rect2D scissor = {};
        scissor.offset.x = 0;
        scissor.offset.y = 0;
        scissor.extent.width = imageExtent.width;
        scissor.extent.height = imageExtent.height;
        command.setScissor(0, 1, &scissor);

        command.draw(3, 1, 0, 0);
        */

        // Draw meshes
        meshPipeline->bind(command);

        vk::Viewport viewport = {};
        viewport.x = 0;
        viewport.y = 0;
        viewport.width = imageExtent.width;
        viewport.height = imageExtent.height;
        viewport.minDepth = 1.f;
        viewport.maxDepth = 0.f;
        command.setViewport(0, 1, &viewport);

        vk::Rect2D scissor = {};
        scissor.offset.x = 0;
        scissor.offset.y = 0;
        scissor.extent.width = imageExtent.width;
        scissor.extent.height = imageExtent.height;
        command.setScissor(0, 1, &scissor);

        GraphicsPushConstants pushConstants;


        /*
        // Draw rectangle mesh
        pushConstants.worldMatrix = glm::mat4{ 1.f };
        pushConstants.vertexBuffer = rectangleMesh.vertexBufferAddress;

        command.pushConstants(meshPipeline->getPipelineLayout(), vk::ShaderStageFlagBits::eVertex, 0, sizeof(GraphicsPushConstants), &pushConstants);
        command.bindIndexBuffer(rectangleMesh.indexBuffer.buffer, 0, vk::IndexType::eUint32);

        command.drawIndexed(6, 1, 0, 0, 0);
        */

        // Texture binding
        vk::DescriptorSet imageSet = getCurrentFrame().frameDescriptors.allocate(singleImageDescriptorLayout);
        {
            DescriptorWriter writer;
            writer.writeImage(0, errorCheckerboardImage.imageView, defaultSamplerNearest,
                vk::ImageLayout::eShaderReadOnlyOptimal, vk::DescriptorType::eCombinedImageSampler);

            writer.updateSet(context->getDevice(), imageSet);
        }
        command.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, meshPipeline->getLayout(), 0, 1, &imageSet, 0, nullptr);


        // Draw test GLTF
        const Mat4 view = glm::translate( Vec3{ 0,0,-5 });
        Mat4 projection = glm::perspective(glm::radians(70.f), static_cast<float>(imageExtent.width) / static_cast<float>(imageExtent.height), 0.1f, 10000.f);
        projection[1][1] *= -1; // Invert the Y direction so that we are more similar to opengl and gltf axis
        pushConstants.worldMatrix = projection * view;
        pushConstants.vertexBuffer = testMeshes[2]->meshBuffers.vertexBufferAddress;

        command.pushConstants(meshPipeline->getLayout(), vk::ShaderStageFlagBits::eVertex, 0, sizeof(GraphicsPushConstants), &pushConstants);
        command.bindIndexBuffer(testMeshes[2]->meshBuffers.indexBuffer.buffer, 0, vk::IndexType::eUint32);

        command.drawIndexed(testMeshes[2]->surfaces[0].count, 1, testMeshes[2]->surfaces[0].startIndex, 0, 0);

        // End of drawing
        command.endRendering();
    }

    void Renderer::createSceneData() {
        array<Vertex, 4> rectVertices;
        rectVertices[0].position = {0.5, -0.5, 0.0};
        rectVertices[1].position = {0.5,0.5, 0};
        rectVertices[2].position = {-0.5,-0.5, 0};
        rectVertices[3].position = {-0.5,0.5, 0};
        rectVertices[0].color = {0,0, 0,1};
        rectVertices[1].color = { 0.5,0.5,0.5 ,1};
        rectVertices[2].color = { 1,0, 0,1 };
        rectVertices[3].color = { 0,1, 0,1 };
        array<uint32_t, 6> rectIndices = {
            0,1,2,
            2,1,3
        };
        rectangleMesh = uploadMesh(rectIndices, rectVertices);

        testMeshes = loadGltfMeshes(this,"assets\\basicmesh.glb").value();

        // Texture
        u32 white = glm::packUnorm4x8(glm::vec4(1, 1, 1, 1));
        whiteImage = Image{ context, immSubmitter, static_cast<void*>(&white), VkExtent3D{ 1, 1, 1 },
            vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eSampled };

        u32 grey = glm::packUnorm4x8(glm::vec4(0.66f, 0.66f, 0.66f, 1));
        greyImage = Image{ context, immSubmitter, static_cast<void*>(&grey), VkExtent3D{ 1, 1, 1 },
            vk::Format::eR8G8B8A8Unorm,vk::ImageUsageFlagBits::eSampled };

        u32 black = glm::packUnorm4x8(glm::vec4(0, 0, 0, 1));
        blackImage = Image{ context, immSubmitter, static_cast<void*>(&black), VkExtent3D{ 1, 1, 1 },
            vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eSampled };

        u32 magenta = glm::packUnorm4x8(glm::vec4(1, 0, 1, 1));
        array<u32, 16 *16 > pixels; // for 16x16 checkerboard texture
        for (int x = 0; x < 16; x++) {
            for (int y = 0; y < 16; y++) {
                pixels[y*16 + x] = ((x % 2) ^ (y % 2)) ? magenta : black;
            }
        }
        errorCheckerboardImage = Image{ context, immSubmitter, pixels.data(), VkExtent3D{16, 16, 1},
            vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eSampled };

        vk::Device device = context->getDevice();
        vk::SamplerCreateInfo samplerInfo {};
        samplerInfo.magFilter = vk::Filter::eNearest;
        samplerInfo.minFilter = vk::Filter::eNearest;
        auto nearestRes = device.createSampler(&samplerInfo, nullptr, &defaultSamplerNearest);

        samplerInfo.magFilter = vk::Filter::eLinear;
        samplerInfo.minFilter = vk::Filter::eLinear;
        auto linearRes = device.createSampler(&samplerInfo, nullptr, &defaultSamplerLinear);

        context->addToMainDeletionQueue([this, device](){
            device.destroySampler(defaultSamplerNearest,nullptr);
            device.destroySampler(defaultSamplerLinear,nullptr);

            whiteImage.destroy(context);
            greyImage.destroy(context);
            blackImage.destroy(context);
            errorCheckerboardImage.destroy(context);
        }, "Default textures and samplers");

        // PBR
        GLTFMetallicRoughness::MaterialResources materialResources;
        //default the material textures
        materialResources.colorImage = whiteImage;
        materialResources.colorSampler = defaultSamplerLinear;
        materialResources.metalRoughImage = whiteImage;
        materialResources.metalRoughSampler = defaultSamplerLinear;

        //set the uniform buffer for the material data
        Buffer materialConstants { context, sizeof(GLTFMetallicRoughness::MaterialConstants), vk::BufferUsageFlagBits::eUniformBuffer, VMA_MEMORY_USAGE_CPU_TO_GPU };

        //write the buffer
        const auto sceneUniformData = static_cast<GLTFMetallicRoughness::MaterialConstants *>(materialConstants.info.pMappedData);
        sceneUniformData->colorFactors = Vec4 {1,1,1,1};
        sceneUniformData->metalRoughFactors = Vec4 {1,0.5,0,0};

        materialResources.dataBuffer = materialConstants.buffer;
        materialResources.dataBufferOffset = 0;

        defaultData = metalRoughMaterial.writeMaterial(device, MaterialPass::MainColor, materialResources, context->getGlobalDescriptorAllocator());
    }

    GPUMeshBuffers Renderer::uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices) {
        const size_t vertexBufferSize = vertices.size() * sizeof(Vertex);
        const size_t indexBufferSize = indices.size() * sizeof(uint32_t);

        GPUMeshBuffers newSurface;

        // Vertex buffer
        newSurface.vertexBuffer = Buffer {context, vertexBufferSize,
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eShaderDeviceAddress,
            VMA_MEMORY_USAGE_GPU_ONLY};

        // Find the address of the vertex buffer
        vk::BufferDeviceAddressInfo deviceAddressInfo {};
        deviceAddressInfo.buffer = newSurface.vertexBuffer.buffer;
        newSurface.vertexBufferAddress = context->getDevice().getBufferAddress(deviceAddressInfo);

        // Index buffer
        newSurface.indexBuffer = Buffer {context,indexBufferSize, vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
            VMA_MEMORY_USAGE_GPU_ONLY};

        // Uploading via staging buffers
        const Buffer staging { context, vertexBufferSize + indexBufferSize, vk::BufferUsageFlagBits::eTransferSrc, VMA_MEMORY_USAGE_CPU_ONLY};
        void* data = staging.info.pMappedData;

        // Copy  buffers
        memcpy(data, vertices.data(), vertexBufferSize);
        memcpy(static_cast<char *>(data) + vertexBufferSize, indices.data(), indexBufferSize);

        immSubmitter.immediateSubmit(context, [&](vk::CommandBuffer cmd) {
            vk::BufferCopy vertexCopy{ 0 };
            vertexCopy.dstOffset = 0;
            vertexCopy.srcOffset = 0;
            vertexCopy.size = vertexBufferSize;

            cmd.copyBuffer(staging.buffer, newSurface.vertexBuffer.buffer, 1, &vertexCopy);

            vk::BufferCopy indexCopy{ 0 };
            indexCopy.dstOffset = 0;
            indexCopy.srcOffset = vertexBufferSize;
            indexCopy.size = indexBufferSize;

            cmd.copyBuffer(staging.buffer, newSurface.indexBuffer.buffer, 1, &indexCopy);
        });

        /*
         * TODO Optimization
         * This pattern is not very efficient, as we are waiting for the GPU command to fully execute before
         * continuing with our CPU side logic. This is something people generally put on a background thread, whose
         * sole job is to execute uploads like this one, and deleting/reusing the staging buffers.
        */
        return newSurface;
    }


    void Renderer::draw() {
        const vk::Device device = context->getDevice();
        FrameData &currentFrameData = getCurrentFrame();

        if (resizeRequested) {
            context->resizeSwapchain();
            resizeRequested = false;
        }

        // Wait for previous frame
        vk::Result fenceResult = device.waitForFences(1, &currentFrameData.renderFence, true, 1000000000);
        currentFrameData.deletionQueue.flush();
        currentFrameData.frameDescriptors.clear();
        const auto res = device.resetFences(1, &currentFrameData.renderFence);

        // Request image from the swapchain
        u32 imageIndex;
        vk::Result result = device.acquireNextImageKHR(*context->getSwapchain()->getSwapchain(), 1000000000,
                                                       currentFrameData.imageAvailableSemaphore, nullptr, &imageIndex);
        if (result == vk::Result::eErrorOutOfDateKHR) {
            resizeRequested = true;
            return;
        }

        const vk::CommandBuffer command = currentFrameData.mainCommandBuffer;
        command.reset();

        // Record command buffer
        vk::CommandBufferBeginInfo beginInfo = graphics::commandBufferBeginInfo(
            vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

        vk::Extent2D drawExtent;
        Image& drawImage = context->getDrawImage();
        Image& depthImage = context->getDepthImage();

        auto swapchainExtent = context->getSwapchain()->getExtent();
        drawExtent.width = std::min(swapchainExtent.width, drawImage.imageExtent.width) / renderScale;
        drawExtent.height = std::min(swapchainExtent.height, drawImage.imageExtent.height) / renderScale;

        command.begin(beginInfo);

        // Make the swapchain image into writeable mode before rendering
        graphics::transitionImage(command, drawImage.image,
                                  vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral);

        drawBackground(command);

        // Transition draw image to color attachment optimal for geometry rendering
        graphics::transitionImage(command, drawImage.image, vk::ImageLayout::eGeneral, vk::ImageLayout::eColorAttachmentOptimal);
        graphics::transitionImage(command, depthImage.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthAttachmentOptimal);

        drawGeometry(command);

        // Transition the draw image and the swapchain image into their correct transfer layouts
        graphics::transitionImage(command, drawImage.image, vk::ImageLayout::eColorAttachmentOptimal,
                                  vk::ImageLayout::eTransferSrcOptimal);
        graphics::transitionImage(command, context->getSwapchain()->getImages()[imageIndex],
                                  vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);

        // Execute a copy from the draw image into the swapchain
        graphics::copyImageToImage(command, drawImage.image, context->getSwapchain()->getImages()[imageIndex],
                                   drawExtent, context->getSwapchain()->getExtent());

        // Transition swapchain image to color attachment for ImGui rendering
        graphics::transitionImage(command, context->getSwapchain()->getImages()[imageIndex],
                                  vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eColorAttachmentOptimal);

        // Render ImGui on top of the swapchain image
        vk::RenderingAttachmentInfo colorAttachment = graphics::attachmentInfo(
            context->getSwapchain()->getImageViews()[imageIndex], nullptr, vk::ImageLayout::eColorAttachmentOptimal);
        vk::RenderingInfo renderInfo = graphics::renderingInfo(vk::Rect2D({0, 0}, context->getSwapchain()->getExtent()),
                                                               &colorAttachment);

        command.beginRendering(renderInfo);
        drawImGui(command);
        command.endRendering();

        // Set swapchain image layout to Present so we can show it on the screen
        graphics::transitionImage(command, context->getSwapchain()->getImages()[imageIndex],
                                  vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::ePresentSrcKHR);

        // Finalize the command buffer (we can no longer add commands, but it can now be executed)
        command.end();

        /* Prepare the submission to the queue.
         * We want to wait on the imageAvailableSemaphore, as that semaphore is signaled when the swapchain is ready
         * We will signal the renderFinishedSemaphore, to signal that rendering has finished
         */

        const vk::CommandBufferSubmitInfo commandInfo = graphics::commandBufferSubmitInfo(command);
        vk::SemaphoreSubmitInfo waitInfo = graphics::semaphoreSubmitInfo(
            currentFrameData.imageAvailableSemaphore, vk::PipelineStageFlagBits2::eColorAttachmentOutput);
        vk::SemaphoreSubmitInfo signalInfo = graphics::semaphoreSubmitInfo(
            renderFinishedSemaphores[imageIndex], vk::PipelineStageFlagBits2::eAllGraphics);
        const vk::SubmitInfo2 submit = graphics::submitInfo(&commandInfo, &signalInfo, &waitInfo);

        /* Submit command buffer to the queue and execute it.
         * renderFence will now block until the graphic commands finish execution
        */
        vk::Result submitResult = context->getGraphicsQueue().submit2(1, &submit, currentFrameData.renderFence);

        /* Prepare present
         * This will put the image we just rendered to into the visible window.
         * we want to wait on the renderFinishedSemaphore for that, as its necessary that
         * drawing commands have finished before the image is displayed to the user.
        */
        vk::PresentInfoKHR presentInfo = {};
        presentInfo.pNext = nullptr;
        presentInfo.pSwapchains = context->getSwapchain()->getSwapchain();
        presentInfo.swapchainCount = 1;

        presentInfo.pWaitSemaphores = &renderFinishedSemaphores[imageIndex];
        presentInfo.waitSemaphoreCount = 1;

        presentInfo.pImageIndices = &imageIndex;

        vk::Result queueResult = context->getGraphicsQueue().presentKHR(&presentInfo);
        if (queueResult == vk::Result::eErrorOutOfDateKHR) {
            resizeRequested = true;
            return;
        }

        // Increase the number of frames drawn
        frameNumber++;
    }

    void Renderer::initImGui() {
        // 1: Create descriptor pool for ImGui
        vk::DescriptorPoolSize pool_sizes[] =
        {
            {vk::DescriptorType::eSampler, 1000},
            {vk::DescriptorType::eCombinedImageSampler, 1000},
            {vk::DescriptorType::eSampledImage, 1000},
            {vk::DescriptorType::eStorageImage, 1000},
            {vk::DescriptorType::eUniformTexelBuffer, 1000},
            {vk::DescriptorType::eStorageTexelBuffer, 1000},
            {vk::DescriptorType::eUniformBuffer, 1000},
            {vk::DescriptorType::eStorageBuffer, 1000},
            {vk::DescriptorType::eUniformBufferDynamic, 1000},
            {vk::DescriptorType::eStorageBufferDynamic, 1000},
            {vk::DescriptorType::eInputAttachment, 1000}
        };

        vk::DescriptorPoolCreateInfo pool_info = {};
        pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        pool_info.maxSets = 1000;
        pool_info.poolSizeCount = std::size(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;

        imguiDescriptorPool = context->getDevice().createDescriptorPool(pool_info);

        // 2: Initialize ImGui library
        ImGui::CreateContext();

        // 3: Initialize ImGui for SDL3
        ImGui_ImplSDL3_InitForVulkan(context->getWindow());

        // 4: Initialize ImGui for Vulkan (using dynamic rendering)
        ImGui_ImplVulkan_InitInfo init_info = {};
        init_info.Instance = static_cast<VkInstance>(context->getInstance());
        init_info.PhysicalDevice = static_cast<VkPhysicalDevice>(context->getPhysicalDevice());
        init_info.Device = static_cast<VkDevice>(context->getDevice());
        init_info.Queue = static_cast<VkQueue>(context->getGraphicsQueue());
        init_info.DescriptorPool = static_cast<VkDescriptorPool>(imguiDescriptorPool);
        init_info.MinImageCount = 3;
        init_info.ImageCount = 3;
        init_info.UseDynamicRendering = true;

        // Dynamic rendering setup
        VkPipelineRenderingCreateInfoKHR pipelineRenderingInfo = {};
        pipelineRenderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        const auto colorAttachmentFormat = static_cast<VkFormat>(context->getSwapchain()->getImageFormat());
        pipelineRenderingInfo.colorAttachmentCount = 1;
        pipelineRenderingInfo.pColorAttachmentFormats = &colorAttachmentFormat;

        init_info.PipelineInfoMain.PipelineRenderingCreateInfo = pipelineRenderingInfo;

        ImGui_ImplVulkan_Init(&init_info);
    }

    float Renderer::getMinRenderScale() const {
        // Calculate min scale to avoid exceeding drawImage size
        auto drawImageExtent = context->getDrawImage().imageExtent;
        auto swapchainExtent = context->getSwapchain()->getExtent();

        // Formula: size = min(swap, draw) / scale
        // We want: scale >= min(swap, draw) / draw
        float minScaleX = static_cast<float>(std::min(swapchainExtent.width, drawImageExtent.width)) / static_cast<float>(drawImageExtent.width);
        float minScaleY = static_cast<float>(std::min(swapchainExtent.height, drawImageExtent.height)) / static_cast<float>(drawImageExtent.height);

        return std::max(minScaleX, minScaleY);
    }

    void Renderer::drawImGui(vk::CommandBuffer commandBuffer) {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        /**
        // Simple Hello World window
        ImGui::Begin("Hello ImGui");
        ImGui::Text("Hello, World!");
        ImGui::End();
        */

        if (ImGui::Begin("background")) {
            const float minScale = getMinRenderScale();
            if (renderScale < minScale) renderScale = minScale;
            ImGui::SliderFloat("Render Scale", &renderScale, minScale, 1.f);

            ComputeEffect &selected = backgroundEffects[currentBackgroundEffect];
            ImGui::Text("Selected effect: %s", selected.name);
            ImGui::SliderInt("Effect Index", &currentBackgroundEffect, 0, backgroundEffects.size() - 1);

            ImGui::InputFloat4("data1", reinterpret_cast<float*>(&selected.data.data1));
            ImGui::InputFloat4("data2", reinterpret_cast<float*>(&selected.data.data2));
            ImGui::InputFloat4("data3", reinterpret_cast<float*>(&selected.data.data3));
            ImGui::InputFloat4("data4", reinterpret_cast<float*>(&selected.data.data4));
        }
        ImGui::End();

        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
    }
} // namespace graphics
