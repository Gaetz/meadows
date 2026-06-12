/**
 * @file Image.cpp
 * @brief Implementation of GPU image management with VMA.
 *
 * This file handles image creation, memory allocation, data upload,
 * and mipmap generation for Vulkan images.
 */

#include "Image.h"

#include "Buffer.h"
#include "Utils.hpp"
#include "VulkanContext.h"
#include "VulkanInit.hpp"

namespace graphics {

    // =========================================================================
    // Constructor: Empty Image (auto mipmap calculation)
    // =========================================================================

    Image::Image(VulkanContext *context, vk::Extent3D size, vk::Format format, vk::ImageUsageFlags usage,
                 bool mipmapped) : context(context), allocation(nullptr), imageExtent(size), imageFormat(format) {
        image = nullptr;
        imageView = nullptr;

        // Create the image info structure
        VkImageCreateInfo img_info = graphics::imageCreateInfo(format, usage, size);

        // Calculate mip levels if requested
        // Formula: floor(log2(max(width, height))) + 1
        // Example: 1024x1024 -> 11 mip levels (1024, 512, 256, ..., 1)
        if (mipmapped) {
            img_info.mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(size.width, size.height)))) + 1;
        }

        // Always allocate images on dedicated GPU memory (VRAM)
        // This gives the best performance for rendering
        VmaAllocationCreateInfo allocinfo {};
        allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        allocinfo.requiredFlags = static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        // Allocate and create the image through VMA
        VkImage newImage = VK_NULL_HANDLE;
        vmaCreateImage(context->getAllocator(), &img_info, &allocinfo, &newImage, &allocation, nullptr);
        image = newImage;

        // Determine the correct aspect flag based on format
        // Depth images use eDepth, color images use eColor
        vk::ImageAspectFlags aspectFlag = vk::ImageAspectFlagBits::eColor;
        if (format == vk::Format::eD32Sfloat || format == vk::Format::eD16Unorm) {
            aspectFlag = vk::ImageAspectFlagBits::eDepth;
        }

        // Create the image view - this is what shaders actually bind to
        VkImageViewCreateInfo view_info = graphics::imageViewCreateInfo(format, image, aspectFlag);
        view_info.subresourceRange.levelCount = img_info.mipLevels;

        VkImageView newView = VK_NULL_HANDLE;
        vkCreateImageView(context->getDevice(), &view_info, nullptr, &newView);
        imageView = newView;
    }

    // =========================================================================
    // Constructor: Empty Image (explicit mip levels)
    // =========================================================================

    Image::Image(VulkanContext *context, vk::Extent3D size, vk::Format format, vk::ImageUsageFlags usage,
                 uint32_t mipLevels) : context(context), allocation(nullptr), imageExtent(size), imageFormat(format) {
        image = nullptr;
        imageView = nullptr;

        VkImageCreateInfo img_info = graphics::imageCreateInfo(format, usage, size);
        img_info.mipLevels = mipLevels;

        // GPU-only memory for best performance
        VmaAllocationCreateInfo allocinfo {};
        allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        allocinfo.requiredFlags = static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        VkImage newImage = VK_NULL_HANDLE;
        vmaCreateImage(context->getAllocator(), &img_info, &allocinfo, &newImage, &allocation, nullptr);
        image = newImage;

        // Determine aspect flag (depth vs color)
        vk::ImageAspectFlags aspectFlag = vk::ImageAspectFlagBits::eColor;
        if (format == vk::Format::eD32Sfloat || format == vk::Format::eD16Unorm) {
            aspectFlag = vk::ImageAspectFlagBits::eDepth;
        }

        VkImageViewCreateInfo view_info = graphics::imageViewCreateInfo(format, image, aspectFlag);
        view_info.subresourceRange.levelCount = mipLevels;

        VkImageView newView = VK_NULL_HANDLE;
        vkCreateImageView(context->getDevice(), &view_info, nullptr, &newView);
        imageView = newView;
    }

    // =========================================================================
    // Constructor: Image with Data Upload
    // =========================================================================

    Image::Image(VulkanContext *context, ImmediateSubmitter& submitter, void *data, vk::Extent3D size, vk::Format format, vk::ImageUsageFlags usage, bool mipmapped)
        // First, create the image with transfer flags added (needed for upload and mipmap generation)
        : Image(context, size, format, usage | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc, mipmapped) {

        // Calculate data size (assuming 4 bytes per pixel - RGBA)
        size_t data_size = static_cast<size_t>(size.depth) * size.width * size.height * 4;

        // Create a staging buffer in CPU-visible memory
        // The GPU can't read from CPU memory directly, so we need this intermediate buffer
        Buffer uploadbuffer {context, data_size, vk::BufferUsageFlagBits::eTransferSrc, VMA_MEMORY_USAGE_CPU_TO_GPU};

        // Copy pixel data to the staging buffer
        memcpy(uploadbuffer.info.pMappedData, data, data_size);

        // Execute GPU commands to transfer data and set up the image
        submitter.immediateSubmit(context, [&](VkCommandBuffer cmd) {
            // Step 1: Transition image to transfer-optimal layout
            // Images must be in the correct layout for each operation
            graphics::transitionImage(cmd, this->image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal);

            // Step 2: Set up the copy region
            VkBufferImageCopy copyRegion = {};
            copyRegion.bufferOffset = 0;
            copyRegion.bufferRowLength = 0;   // Tightly packed
            copyRegion.bufferImageHeight = 0; // Tightly packed

            copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.imageSubresource.mipLevel = 0;       // Copy to mip level 0
            copyRegion.imageSubresource.baseArrayLayer = 0;
            copyRegion.imageSubresource.layerCount = 1;
            copyRegion.imageExtent = size;

            // Step 3: Copy from staging buffer to image
            vkCmdCopyBufferToImage(cmd, uploadbuffer.buffer, this->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                &copyRegion);

            // Step 4: Generate mipmaps or transition to shader-read layout
            if (mipmapped) {
                // generateMipmaps also transitions to shader-read layout
                graphics::generateMipmaps(cmd, this->image, vk::Extent2D { size.width, size.height });
            } else {
                // Just transition to shader-read layout
                graphics::transitionImage(cmd, this->image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eReadOnlyOptimal);
            }
        });
        // Staging buffer is automatically destroyed when it goes out of scope
    }

    // =========================================================================
    // Move Constructor
    // =========================================================================

    Image::Image(Image&& other) noexcept
        : context(other.context), image(other.image), allocation(other.allocation),
          imageView(other.imageView), imageExtent(other.imageExtent), imageFormat(other.imageFormat) {
        // Invalidate the moved-from object to prevent double-destruction
        other.image = nullptr;
        other.imageView = nullptr;
        other.allocation = nullptr;
    }

    // =========================================================================
    // Move Assignment
    // =========================================================================

    Image& Image::operator=(Image&& other) noexcept {
        if (this != &other) {
            // Transfer ownership
            context = other.context;
            image = other.image;
            allocation = other.allocation;
            imageView = other.imageView;
            imageExtent = other.imageExtent;
            imageFormat = other.imageFormat;

            // Invalidate moved-from object
            other.image = nullptr;
            other.imageView = nullptr;
            other.allocation = nullptr;
        }
        return *this;
    }

    // =========================================================================
    // Cleanup
    // =========================================================================

    void Image::destroy(const VulkanContext* ctx) {
        if (ctx) {
            // Destroy image view first
            if (imageView) {
                ctx->getDevice().destroyImageView(imageView);
                imageView = nullptr;
            }
            // Then destroy image and free memory through VMA
            if (image && allocation) {
                vmaDestroyImage(ctx->getAllocator(), image, allocation);
                image = nullptr;
                allocation = nullptr;
            }
        }
    }
}
