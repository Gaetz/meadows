/**
 * @file PipelineCompute.cpp
 * @brief Implementation of the Vulkan Compute Pipeline wrapper.
 *
 * Compute pipelines enable general-purpose GPU computing (GPGPU) without
 * the overhead of a full graphics pipeline.
 */

#include "PipelineCompute.h"

#include "VulkanContext.h"
#include "Utils.hpp"
#include "VulkanInit.hpp"
#include "BasicServices/File.h"

using services::File;

namespace graphics {

    // =========================================================================
    // Constructor
    // =========================================================================

    PipelineCompute::PipelineCompute(VulkanContext *context, const str& compFilepath, const vk::PipelineLayout computePipelineLayout) {
        this->context = context;
        this->computePipelineLayout = computePipelineLayout;
        createComputePipeline(compFilepath);
    }

    // =========================================================================
    // Pipeline Binding
    // =========================================================================

    void PipelineCompute::bind(vk::CommandBuffer commandBuffer) {
        // Bind as compute pipeline (not graphics)
        // After this, dispatch commands will use this pipeline
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, computePipeline);
    }

    // =========================================================================
    // Pipeline Creation
    // =========================================================================

    void PipelineCompute::createComputePipeline(const str& compFilepath) {
        // Step 1: Load the compiled shader code (SPIR-V binary)
        const auto compCode = File::readBinary(compFilepath);

        // Step 2: Create a shader module from the binary
        // A shader module is Vulkan's wrapper around SPIR-V bytecode
        compShaderModule = createShaderModule(compCode, context->getDevice());

        // Step 3: Create the shader stage info
        // For compute, there's only one stage (unlike vertex+fragment for graphics)
        const vk::PipelineShaderStageCreateInfo stageInfo = graphics::shaderStageCreateInfo(vk::ShaderStageFlagBits::eCompute, compShaderModule);

        // Step 4: Create the compute pipeline
        // Much simpler than graphics pipelines - just needs layout and shader stage
        vk::ComputePipelineCreateInfo computePipelineCreateInfo{};
        computePipelineCreateInfo.layout = computePipelineLayout;  // Descriptor sets and push constants
        computePipelineCreateInfo.stage = stageInfo;               // The compute shader

        computePipeline = context->getDevice().createComputePipeline(nullptr, computePipelineCreateInfo).value;

        // Step 5: Destroy the shader module - it's baked into the pipeline now
        // Keeping it around would just waste memory
        context->getDevice().destroyShaderModule(compShaderModule);

        // Step 6: Register the pipeline for cleanup when context is destroyed
        // We capture copies of handles because the lambda may outlive this object
        vk::Device contextDevice = context->getDevice();
        vk::Pipeline pipelineCopy = computePipeline;
        context->addToMainDeletionQueue([pipelineCopy, contextDevice]() {
                contextDevice.destroyPipeline(pipelineCopy);
            }
        , "computePipeline");
    }
}
