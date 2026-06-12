/**
 * @file Image.h
 * @brief GPU image wrapper with automatic memory management via VMA.
 */

#pragma once
#include <vk_mem_alloc.h>

#include "Types.h"

namespace graphics {

    class VulkanContext;
    class ImmediateSubmitter;

    /**
     * @class Image
     * @brief Manages a Vulkan image with its memory allocation and image view.
     *
     * ## What is a Vulkan Image?
     * A VkImage is GPU memory organized as a 2D or 3D grid of pixels (texels).
     * Unlike buffers (linear memory), images support:
     * - Texture filtering (bilinear, trilinear, anisotropic)
     * - Mipmapping (multiple resolution levels)
     * - Various layouts optimized for different uses
     *
     * ## What is an Image View?
     * A VkImageView is a "lens" through which shaders see an image. It specifies:
     * - Which mip levels to access
     * - Which array layers to access
     * - How to interpret the format (e.g., view RGB as grayscale)
     *
     * ## What is VMA (Vulkan Memory Allocator)?
     * VMA is a library that simplifies Vulkan memory management. Instead of
     * manually allocating memory and binding it to images, VMA handles:
     * - Memory type selection (GPU-only, CPU-visible, etc.)
     * - Memory pooling for efficiency
     * - Defragmentation
     *
     * ## Memory allocation strategy:
     * This class always allocates images in GPU-only memory (DEVICE_LOCAL)
     * for maximum performance. Data upload requires a staging buffer.
     *
     * ## Typical usage:
     * @code
     * // Create an empty image for rendering
     * Image renderTarget(context, {1920, 1080, 1}, vk::Format::eR8G8B8A8Srgb,
     *                    vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled);
     *
     * // Create a texture from data (with mipmaps)
     * Image texture(context, submitter, pixelData, {512, 512, 1}, vk::Format::eR8G8B8A8Srgb,
     *               vk::ImageUsageFlagBits::eSampled, true);
     * @endcode
     */
    class Image {
    public:
        Image() = default;

        /**
         * @brief Creates an empty image on the GPU.
         * @param context The Vulkan context.
         * @param size Image dimensions (width, height, depth). Use depth=1 for 2D images.
         * @param format Pixel format (e.g., eR8G8B8A8Srgb, eD32Sfloat for depth).
         * @param usage How the image will be used (sampled, color attachment, etc.).
         * @param mipmapped If true, automatically calculates and allocates mip levels.
         *
         * The image is allocated in GPU-only memory for maximum performance.
         */
        Image(VulkanContext* context, vk::Extent3D size, vk::Format format, vk::ImageUsageFlags usage, bool mipmapped = false);

        /**
         * @brief Creates an empty image with explicit mip level count.
         * @param context The Vulkan context.
         * @param size Image dimensions.
         * @param format Pixel format.
         * @param usage Usage flags.
         * @param mipLevels Exact number of mip levels to allocate.
         */
        Image(VulkanContext* context, vk::Extent3D size, vk::Format format, vk::ImageUsageFlags usage, uint32_t mipLevels);

        /**
         * @brief Creates an image and uploads pixel data to it.
         * @param context The Vulkan context.
         * @param submitter For immediate GPU command execution.
         * @param data Pointer to pixel data (assumed RGBA, 4 bytes per pixel).
         * @param size Image dimensions.
         * @param format Pixel format.
         * @param usage Usage flags (eTransferDst is added automatically).
         * @param mipmapped If true, generates mipmaps after upload.
         *
         * This constructor:
         * 1. Creates a staging buffer with the pixel data
         * 2. Transitions the image to transfer-optimal layout
         * 3. Copies data from buffer to image
         * 4. Generates mipmaps (if requested) or transitions to shader-read layout
         */
        Image(VulkanContext* context, ImmediateSubmitter& submitter, void* data, vk::Extent3D size, vk::Format format, vk::ImageUsageFlags usage, bool mipmapped = false);

        ~Image() = default;

        /**
         * @brief Destroys the image, image view, and frees GPU memory.
         * @param context The Vulkan context.
         *
         * @note Must be called explicitly - destructor doesn't auto-cleanup
         *       because Image supports both copy and move semantics.
         */
        void destroy(const VulkanContext* context);

        // =====================================================================
        // Image Resources
        // =====================================================================

        VulkanContext* context { nullptr };  ///< Vulkan context reference
        vk::Image image;                      ///< The Vulkan image handle
        VmaAllocation allocation;             ///< VMA allocation handle (for memory management)
        vk::ImageView imageView;              ///< View for shader access
        vk::Extent3D imageExtent;             ///< Image dimensions
        vk::Format imageFormat;               ///< Pixel format

        // =====================================================================
        // Move Semantics
        // =====================================================================

        /// Move constructor - transfers ownership of GPU resources
        Image(Image&& other) noexcept;

        /// Move assignment - transfers ownership of GPU resources
        Image& operator=(Image&& other) noexcept;

        // Copy is allowed but be careful - both copies share the same GPU resources!
        Image(const Image&) = default;
        Image& operator=(const Image&) = default;
    };
}