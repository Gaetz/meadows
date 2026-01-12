#pragma once

#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "Image.h"

namespace graphics {

    class Renderer;
    class VulkanContext;
    struct ImmediateSubmitter;

    // KTX1 file header structure
    struct KTX1Header {
        uint8_t identifier[12];
        uint32_t endianness;
        uint32_t glType;
        uint32_t glTypeSize;
        uint32_t glFormat;
        uint32_t glInternalFormat;
        uint32_t glBaseInternalFormat;
        uint32_t pixelWidth;
        uint32_t pixelHeight;
        uint32_t pixelDepth;
        uint32_t numberOfArrayElements;
        uint32_t numberOfFaces;
        uint32_t numberOfMipmapLevels;
        uint32_t bytesOfKeyValueData;
    };

    // Result of loading a KTX file
    struct KTXLoadResult {
        std::vector<uint8_t> data;
        uint32_t width;
        uint32_t height;
        uint32_t mipLevels;
        vk::Format format;
        std::vector<size_t> mipOffsets;  // Offset to each mip level
        std::vector<size_t> mipSizes;    // Size of each mip level
    };

    // Load a KTX1 file and return the texture data
    std::optional<KTXLoadResult> loadKTXFile(const std::string& filePath);

    // Load a KTX file directly as a Vulkan Image
    std::optional<Image> loadKTXImage(Renderer* renderer, const std::string& filePath);

} // namespace graphics
