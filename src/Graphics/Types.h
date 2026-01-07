#pragma once

#include <vulkan/vulkan.hpp>
#include <glm/glm.hpp>
#include "../Defines.h"
#include <optional>
#include <vk_mem_alloc.h>


namespace graphics
{
    struct Vertex
    {
        glm::vec3 position;
        float uvX;
        glm::vec3 normal;
        float uvY;
        glm::vec4 color;
    };

    // Push constants for our mesh object draws
    struct GraphicsPushConstants
    {
        glm::mat4 worldMatrix;
        vk::DeviceAddress vertexBuffer;
    };

    struct UniformBufferObject
    {
        Mat4 model;
        Mat4 view;
        Mat4 proj;
    };

    struct AllocatedImage
    {
        vk::Image image;
        VmaAllocation allocation;
        vk::ImageView imageView;
        vk::Extent3D imageExtent;
        vk::Format imageFormat;
    };

    struct QueueFamilyIndices
    {
        std::optional<uint32_t> graphicsFamily;
        std::optional<uint32_t> presentFamily;

        bool isComplete() { return graphicsFamily.has_value() && presentFamily.has_value(); }
    };

    struct SwapChainSupportDetails
    {
        vk::SurfaceCapabilitiesKHR capabilities;
        std::vector<vk::SurfaceFormatKHR> formats;
        std::vector<vk::PresentModeKHR> presentModes;
    };

    struct ComputePushConstants
    {
        Vec4 data1;
        Vec4 data2;
        Vec4 data3;
        Vec4 data4;
    };
} // namespace graphics
