#include "KTXLoader.h"
#include "Renderer.h"
#include "VulkanContext.h"
#include "../BasicServices/Log.h"
#include "../BasicServices/File.h"
#include <fstream>
#include <cstring>

using services::Log;

namespace graphics {

    // KTX1 identifier
    static const uint8_t KTX1_IDENTIFIER[12] = {
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
    };

    // Map OpenGL internal format to Vulkan format
    static vk::Format glInternalFormatToVk(uint32_t glInternalFormat) {
        // Common formats used in Sascha Willems examples
        switch (glInternalFormat) {
            // RGBA8
            case 0x8058: // GL_RGBA8
                return vk::Format::eR8G8B8A8Unorm;
            case 0x8C43: // GL_SRGB8_ALPHA8
                return vk::Format::eR8G8B8A8Srgb;

            // RGB8
            case 0x8051: // GL_RGB8
                return vk::Format::eR8G8B8Unorm;
            case 0x8C41: // GL_SRGB8
                return vk::Format::eR8G8B8Srgb;

            // Compressed formats (BC/DXT)
            case 0x83F0: // GL_COMPRESSED_RGB_S3TC_DXT1_EXT
                return vk::Format::eBc1RgbUnormBlock;
            case 0x83F1: // GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
                return vk::Format::eBc1RgbaUnormBlock;
            case 0x83F2: // GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
                return vk::Format::eBc2UnormBlock;
            case 0x83F3: // GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
                return vk::Format::eBc3UnormBlock;

            // More uncompressed formats
            case 0x1908: // GL_RGBA (sized as RGBA8)
                return vk::Format::eR8G8B8A8Unorm;
            case 0x1907: // GL_RGB
                return vk::Format::eR8G8B8Unorm;

            default:
                Log::Warn("Unknown GL internal format: 0x%X, defaulting to R8G8B8A8Unorm", glInternalFormat);
                return vk::Format::eR8G8B8A8Unorm;
        }
    }

    std::optional<KTXLoadResult> loadKTXFile(const std::string& filePath) {
        auto fsPath = services::File::getFileSystemPath(filePath);
        std::ifstream file(fsPath, std::ios::binary | std::ios::ate);

        if (!file.is_open()) {
            Log::Error("Failed to open KTX file: %s", filePath.c_str());
            return std::nullopt;
        }

        size_t fileSize = file.tellg();
        file.seekg(0, std::ios::beg);

        // Read header
        KTX1Header header;
        file.read(reinterpret_cast<char*>(&header), sizeof(KTX1Header));

        // Verify identifier
        if (memcmp(header.identifier, KTX1_IDENTIFIER, 12) != 0) {
            Log::Error("Invalid KTX1 file identifier: %s", filePath.c_str());
            return std::nullopt;
        }

        // Check endianness
        if (header.endianness != 0x04030201) {
            Log::Error("KTX file has wrong endianness (big-endian not supported): %s", filePath.c_str());
            return std::nullopt;
        }

        Log::Debug("KTX: %s - %dx%d, %d mip levels, glInternalFormat=0x%X",
            filePath.c_str(), header.pixelWidth, header.pixelHeight,
            header.numberOfMipmapLevels, header.glInternalFormat);

        // Skip key-value data
        file.seekg(sizeof(KTX1Header) + header.bytesOfKeyValueData, std::ios::beg);

        KTXLoadResult result;
        result.width = header.pixelWidth;
        result.height = header.pixelHeight;
        result.mipLevels = header.numberOfMipmapLevels > 0 ? header.numberOfMipmapLevels : 1;
        result.format = glInternalFormatToVk(header.glInternalFormat);

        // Calculate total data size and read mip levels
        size_t totalDataSize = 0;
        std::vector<std::pair<size_t, size_t>> mipInfo; // offset in file, size

        size_t currentOffset = file.tellg();

        for (uint32_t mip = 0; mip < result.mipLevels; mip++) {
            uint32_t imageSize;
            file.read(reinterpret_cast<char*>(&imageSize), sizeof(uint32_t));

            mipInfo.push_back({currentOffset + sizeof(uint32_t), imageSize});
            totalDataSize += imageSize;

            // Skip to next mip level (imageSize + padding to 4-byte boundary)
            size_t paddedSize = (imageSize + 3) & ~3;
            file.seekg(paddedSize, std::ios::cur);
            currentOffset = file.tellg();
        }

        // Now read all the data
        result.data.resize(totalDataSize);
        size_t writeOffset = 0;

        for (uint32_t mip = 0; mip < result.mipLevels; mip++) {
            file.seekg(mipInfo[mip].first, std::ios::beg);
            file.read(reinterpret_cast<char*>(result.data.data() + writeOffset), mipInfo[mip].second);

            result.mipOffsets.push_back(writeOffset);
            result.mipSizes.push_back(mipInfo[mip].second);

            writeOffset += mipInfo[mip].second;
        }

        return result;
    }

    std::optional<Image> loadKTXImage(Renderer* renderer, const std::string& filePath) {
        auto ktxResult = loadKTXFile(filePath);
        if (!ktxResult.has_value()) {
            return std::nullopt;
        }

        const auto& ktx = *ktxResult;
        VulkanContext* context = renderer->getContext();
        vk::Device device = context->getDevice();

        // Create the image with mipmaps
        vk::Extent3D imageExtent = { ktx.width, ktx.height, 1 };

        Image image(context, imageExtent, ktx.format,
            vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
            ktx.mipLevels);

        // Create staging buffer
        vk::DeviceSize bufferSize = ktx.data.size();
        Buffer staging(context, bufferSize, vk::BufferUsageFlagBits::eTransferSrc, VMA_MEMORY_USAGE_CPU_ONLY);

        // Copy data to staging buffer
        memcpy(staging.info.pMappedData, ktx.data.data(), bufferSize);

        // Upload to GPU
        renderer->getImmediateSubmitter()->immediateSubmit(context, [&](vk::CommandBuffer cmd) {
            // Transition image to transfer dst
            vk::ImageMemoryBarrier barrier{};
            barrier.oldLayout = vk::ImageLayout::eUndefined;
            barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image.image;
            barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = ktx.mipLevels;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            barrier.srcAccessMask = vk::AccessFlagBits::eNone;
            barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;

            cmd.pipelineBarrier(
                vk::PipelineStageFlagBits::eTopOfPipe,
                vk::PipelineStageFlagBits::eTransfer,
                {}, {}, {}, barrier
            );

            // Copy each mip level
            for (uint32_t mip = 0; mip < ktx.mipLevels; mip++) {
                uint32_t mipWidth = std::max(1u, ktx.width >> mip);
                uint32_t mipHeight = std::max(1u, ktx.height >> mip);

                vk::BufferImageCopy region{};
                region.bufferOffset = ktx.mipOffsets[mip];
                region.bufferRowLength = 0;
                region.bufferImageHeight = 0;
                region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
                region.imageSubresource.mipLevel = mip;
                region.imageSubresource.baseArrayLayer = 0;
                region.imageSubresource.layerCount = 1;
                region.imageOffset = vk::Offset3D{0, 0, 0};
                region.imageExtent = vk::Extent3D{mipWidth, mipHeight, 1};

                cmd.copyBufferToImage(staging.buffer, image.image,
                    vk::ImageLayout::eTransferDstOptimal, region);
            }

            // Transition to shader read optimal
            barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
            barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
            barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
            barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;

            cmd.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eFragmentShader,
                {}, {}, {}, barrier
            );
        });

        Log::Info("Loaded KTX texture: %s (%dx%d, %d mips)",
            filePath.c_str(), ktx.width, ktx.height, ktx.mipLevels);

        return image;
    }

} // namespace graphics
