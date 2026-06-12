#pragma once
#include <vector>

#include "Types.h"

namespace graphics {
    class VulkanContext;
    class MaterialPipeline;

    /**
     * @class PipelineBuilder
     * @brief Builder pattern for creating Vulkan Graphics Pipelines.
     *
     * ## What is a Graphics Pipeline?
     * A graphics pipeline is the sequence of operations that transforms 3D geometry
     * into 2D pixels on screen. It includes:
     * - Vertex processing (positioning vertices)
     * - Rasterization (converting triangles to fragments)
     * - Fragment processing (coloring pixels)
     * - Output merging (blending, depth testing)
     *
     * ## Why is pipeline creation so complex in Vulkan?
     * Vulkan exposes all the GPU state that was hidden in OpenGL. This gives you
     * more control but requires configuring many settings:
     * - Shaders (vertex, fragment)
     * - Input assembly (how vertices form primitives)
     * - Rasterization (fill mode, culling)
     * - Multisampling (anti-aliasing)
     * - Color blending (transparency)
     * - Depth/stencil testing
     * - Dynamic state (what can change at runtime)
     *
     * ## How this builder helps:
     * Instead of filling a massive VkGraphicsPipelineCreateInfo structure,
     * you call intuitive methods like setPolygonMode() or enableBlendingAlphaBlend().
     * The builder maintains sensible defaults and handles all the boilerplate.
     *
     * ## Typical usage:
     * @code
     * PipelineBuilder builder(context, "shaders/mesh.vert.spv", "shaders/mesh.frag.spv");
     * builder.setInputTopology(vk::PrimitiveTopology::eTriangleList);
     * builder.setPolygonMode(vk::PolygonMode::eFill);
     * builder.setCullMode(vk::CullModeFlagBits::eBack, vk::FrontFace::eCounterClockwise);
     * builder.enableDepthTest(true, vk::CompareOp::eLess);
     * builder.setColorAttachmentFormat(swapchainFormat);
     * builder.pipelineLayout = myLayout;
     * auto pipeline = builder.buildPipeline(device);
     * @endcode
     *
     * @note Pipelines are immutable once created. To change settings, create a new pipeline.
     */
    class PipelineBuilder {
    public:
        VulkanContext* context {nullptr};  ///< Vulkan context for device access

        // =====================================================================
        // Shader Modules
        // =====================================================================
        vk::ShaderModule vertexShaderModule {nullptr};    ///< Vertex shader (processes each vertex)
        vk::ShaderModule fragmentShaderModule {nullptr};  ///< Fragment shader (colors each pixel)

        // =====================================================================
        // Pipeline Configuration State
        // =====================================================================

        /// Shader stages to include in the pipeline (usually vertex + fragment)
        vector<vk::PipelineShaderStageCreateInfo> shaderStages;

        /// How vertices are assembled into primitives (triangles, lines, points)
        vk::PipelineInputAssemblyStateCreateInfo inputAssembly;

        /// Rasterization settings (fill mode, culling, winding order)
        vk::PipelineRasterizationStateCreateInfo rasterizer;

        /// How fragment colors blend with existing framebuffer colors
        vk::PipelineColorBlendAttachmentState colorBlendAttachment;

        /// Multisampling/anti-aliasing settings
        vk::PipelineMultisampleStateCreateInfo multisampling;

        /// The pipeline layout (descriptor sets and push constants)
        vk::PipelineLayout pipelineLayout;

        /// Depth and stencil testing configuration
        vk::PipelineDepthStencilStateCreateInfo depthStencil;

        /// Dynamic rendering info (color/depth attachment formats)
        vk::PipelineRenderingCreateInfo renderInfo;

        /// Formats of color attachments (for dynamic rendering)
        std::vector<vk::Format> colorAttachmentFormats;

        // =====================================================================
        // Special Modes
        // =====================================================================

        /// If true, creates a depth-only pipeline (no color attachments, used for shadow maps)
        bool depthOnlyMode { false };

        /// If true, enables depth bias (prevents shadow acne in shadow mapping)
        bool depthBiasEnable { false };

        /// Optional specialization constants for the fragment shader
        std::optional<vk::SpecializationInfo> fragmentSpecialization;

        // =====================================================================
        // Constructors
        // =====================================================================

        /**
         * @brief Creates an empty pipeline builder.
         * @param context The Vulkan context.
         *
         * You'll need to set shaders manually with setShaders().
         */
        PipelineBuilder(VulkanContext* context);

        /**
         * @brief Creates a pipeline builder and loads shaders from files.
         * @param context The Vulkan context.
         * @param vertFilePath Path to the compiled vertex shader (.spv).
         * @param fragFilePath Path to the compiled fragment shader (.spv).
         */
        PipelineBuilder(VulkanContext* context, const str& vertFilePath, const str& fragFilePath);

        /**
         * @brief Resets the builder to default state.
         *
         * Call this to reuse the builder for creating another pipeline.
         */
        void clear();

        /**
         * @brief Builds the graphics pipeline with the current configuration.
         * @param device The Vulkan logical device.
         * @return A unique pointer to the created MaterialPipeline.
         *
         * @note Remember to set pipelineLayout before calling this!
         */
        [[nodiscard]] uptr<MaterialPipeline> buildPipeline(vk::Device device) const;

        // =====================================================================
        // Shader Configuration
        // =====================================================================

        /**
         * @brief Sets both vertex and fragment shaders.
         * @param vertexShader The vertex shader module.
         * @param fragmentShader The fragment shader module.
         */
        void setShaders(vk::ShaderModule vertexShader, vk::ShaderModule fragmentShader);

        /**
         * @brief Sets only a vertex shader (for depth-only passes).
         * @param vertexShader The vertex shader module.
         */
        void setVertexShaderOnly(vk::ShaderModule vertexShader);

        /**
         * @brief Sets specialization constants for the fragment shader.
         * @param specInfo The specialization info containing constant values.
         *
         * Specialization constants allow compile-time configuration of shaders.
         */
        void setFragmentSpecialization(const vk::SpecializationInfo& specInfo);

        /**
         * @brief Destroys the shader modules.
         * @param device The Vulkan device.
         *
         * Call this after buildPipeline() - the modules are no longer needed.
         */
        void destroyShaderModules(vk::Device device);

        // =====================================================================
        // Input Assembly
        // =====================================================================

        /**
         * @brief Sets how vertices are assembled into primitives.
         * @param topology The primitive topology (eTriangleList, eLineList, etc.).
         *
         * Most common: eTriangleList - every 3 vertices form a triangle.
         */
        void setInputTopology(vk::PrimitiveTopology topology);

        // =====================================================================
        // Rasterization
        // =====================================================================

        /**
         * @brief Sets the polygon fill mode.
         * @param mode How polygons are rendered (eFill, eLine, ePoint).
         *
         * eFill: Normal solid rendering
         * eLine: Wireframe mode
         * ePoint: Vertices as points
         */
        void setPolygonMode(vk::PolygonMode mode);

        /**
         * @brief Sets face culling mode.
         * @param cullMode Which faces to cull (eNone, eFront, eBack, eFrontAndBack).
         * @param frontFace Which winding order is considered "front" (eClockwise, eCounterClockwise).
         *
         * Culling back faces improves performance by not drawing triangles facing away.
         */
        void setCullMode(vk::CullModeFlags cullMode, vk::FrontFace frontFace);

        // =====================================================================
        // Multisampling
        // =====================================================================

        /**
         * @brief Disables multisampling (1 sample per pixel).
         *
         * Multisampling is a form of anti-aliasing. Disabling it is faster but
         * may result in jagged edges.
         */
        void setMultisamplingNone();

        // =====================================================================
        // Color Blending
        // =====================================================================

        /**
         * @brief Disables blending (fragments overwrite existing colors).
         *
         * Use for opaque objects.
         */
        void disableBlending();

        /**
         * @brief Enables additive blending (new color adds to existing).
         *
         * Formula: finalColor = srcColor * srcAlpha + dstColor
         * Use for: Glowing effects, particles, fire
         */
        void enableBlendingAdditive();

        /**
         * @brief Enables standard alpha blending (transparency).
         *
         * Formula: finalColor = srcColor * srcAlpha + dstColor * (1 - srcAlpha)
         * Use for: Transparent objects, UI elements
         */
        void enableBlendingAlphaBlend();

        // =====================================================================
        // Render Targets
        // =====================================================================

        /**
         * @brief Sets a single color attachment format.
         * @param format The image format (e.g., eR8G8B8A8Srgb).
         */
        void setColorAttachmentFormat(vk::Format format);

        /**
         * @brief Sets multiple color attachment formats (for MRT - Multiple Render Targets).
         * @param formats Array of image formats.
         */
        void setColorAttachmentFormats(std::span<vk::Format> formats);

        /**
         * @brief Sets the depth attachment format.
         * @param format The depth format (e.g., eD32Sfloat, eD24UnormS8Uint).
         */
        void setDepthFormat(vk::Format format);

        // =====================================================================
        // Depth Testing
        // =====================================================================

        /**
         * @brief Disables depth testing entirely.
         *
         * Fragments are not tested against the depth buffer.
         * Use for: UI, fullscreen effects, skyboxes
         */
        void disableDepthTest();

        /**
         * @brief Enables depth testing.
         * @param depthWriteEnable Whether to write to the depth buffer.
         * @param compareOp How to compare fragment depth with buffer (eLess, eLessOrEqual, etc.).
         *
         * eLess: Fragment passes if its depth < buffer depth (closer to camera)
         * Disable depth write for transparent objects (they shouldn't occlude).
         */
        void enableDepthTest(bool depthWriteEnable, vk::CompareOp compareOp);

        /**
         * @brief Enables/disables depth bias.
         * @param enable Whether to enable depth bias.
         *
         * Depth bias offsets depth values to prevent z-fighting.
         * Essential for shadow mapping to prevent "shadow acne".
         */
        void setDepthBiasEnable(bool enable);

        /**
         * @brief Enables depth-only rendering mode.
         * @param enable Whether to enable depth-only mode.
         *
         * In depth-only mode, no color attachments are used.
         * Use for: Shadow map generation, depth pre-pass.
         */
        void setDepthOnlyMode(bool enable);
    };
} // namespace graphics
