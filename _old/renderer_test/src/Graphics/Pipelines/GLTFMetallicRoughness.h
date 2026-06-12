/**
 * @file GLTFMetallicRoughness.h
 * @brief PBR material pipeline following the glTF 2.0 metallic-roughness workflow.
 */

#pragma once

#include "Graphics/Types.h"
#include "Graphics/DescriptorWriter.h"
#include "Graphics/MaterialPipeline.h"
#include "Graphics/Image.h"

namespace graphics {
    class DescriptorAllocatorGrowable;
    class Renderer;
}

namespace  graphics::pipelines {

    /**
     * @class GLTFMetallicRoughness
     * @brief Implements the standard PBR metallic-roughness material model from glTF 2.0.
     *
     * ## What is PBR (Physically Based Rendering)?
     * PBR is a shading model that simulates how light interacts with surfaces in a
     * physically plausible way. It produces more realistic results than older models
     * like Phong or Blinn-Phong.
     *
     * ## What is the Metallic-Roughness workflow?
     * This is one of two common PBR workflows (the other being Specular-Glossiness).
     * It uses two main parameters:
     * - **Metallic**: 0 = dielectric (plastic, wood), 1 = metal (gold, steel)
     * - **Roughness**: 0 = smooth/shiny (mirror), 1 = rough/matte (chalk)
     *
     * ## What textures does it use?
     * - **Base Color (Albedo)**: The surface color (RGB) + optional alpha
     * - **Metallic-Roughness**: Metallic in B channel, Roughness in G channel
     *
     * ## Why two pipelines (opaque and transparent)?
     * - **Opaque**: Faster, writes to depth buffer, no blending
     * - **Transparent**: Uses alpha blending, doesn't write depth (to avoid sorting issues)
     *
     * ## Typical usage:
     * @code
     * GLTFMetallicRoughness material;
     * material.buildPipelines(renderer);
     *
     * // For each mesh:
     * MaterialInstance instance = material.writeMaterial(device, MaterialPass::MainColor, resources, allocator);
     * // Use instance.pipeline and instance.materialSet for rendering
     * @endcode
     */
    class GLTFMetallicRoughness {
    public:
        /// Pipeline for opaque geometry (no transparency)
        uptr<MaterialPipeline> opaquePipeline { nullptr };

        /// Pipeline for transparent geometry (alpha blending)
        uptr<MaterialPipeline> transparentPipeline { nullptr };

        /// Descriptor set layout for material data (uniforms + textures)
        vk::DescriptorSetLayout materialLayout { nullptr };

        /// Pipeline layout shared by both pipelines
        vk::PipelineLayout pipelineLayout { nullptr };

        /**
         * @struct MaterialConstants
         * @brief Uniform buffer data sent to the shader.
         *
         * These are material properties that don't come from textures.
         */
        struct MaterialConstants {
            Vec4 colorFactors;        ///< Base color multiplier (RGBA)
            Vec4 metalRoughFactors;   ///< x=metallic, y=roughness multipliers
            glm::vec4 extra[14];      ///< Padding to align to uniform buffer requirements
        };

        /**
         * @struct MaterialResources
         * @brief All resources needed to render with this material.
         *
         * This bundles together textures, samplers, and the uniform buffer
         * containing MaterialConstants.
         */
        struct MaterialResources {
            Image colorImage;              ///< Base color texture
            vk::Sampler colorSampler;      ///< Sampler for the color texture
            Image metalRoughImage;         ///< Metallic-roughness texture
            vk::Sampler metalRoughSampler; ///< Sampler for metallic-roughness
            vk::Buffer dataBuffer;         ///< Buffer containing MaterialConstants
            u32 dataBufferOffset;          ///< Offset into the buffer for this material
        };

        /// Helper for writing descriptor sets
        DescriptorWriter writer;

        /**
         * @brief Creates both opaque and transparent pipelines.
         * @param renderer The renderer (provides context and formats).
         *
         * This creates:
         * - The descriptor set layout for materials
         * - The pipeline layout (scene data + material data)
         * - The opaque pipeline (no blending, depth write)
         * - The transparent pipeline (additive blending, no depth write)
         */
        void buildPipelines(const Renderer *renderer);

        /**
         * @brief Destroys all Vulkan resources.
         * @param device The Vulkan device.
         */
        void clear(vk::Device device);

        /**
         * @brief Creates a material instance ready for rendering.
         * @param device The Vulkan device.
         * @param pass Which render pass (MainColor or Transparent).
         * @param resources The textures and buffers for this material.
         * @param descriptorAllocator Allocator for the descriptor set.
         * @return A MaterialInstance containing the pipeline and descriptor set.
         *
         * This allocates a descriptor set and writes all the resource bindings to it.
         */
        MaterialInstance writeMaterial(vk::Device device, MaterialPass pass, const MaterialResources& resources, DescriptorAllocatorGrowable* descriptorAllocator);
    };

}
