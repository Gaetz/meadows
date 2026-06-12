#pragma once
#define GLM_ENABLE_EXPERIMENTAL

#include <vulkan/vulkan.hpp>
#include <glm/glm.hpp>
#include "../Defines.h"
#include <optional>
#include <vk_mem_alloc.h>

namespace graphics
{
    class MaterialPipeline;

    struct Vertex
    {
        glm::vec3 position;
        float uvX;
        glm::vec3 normal;
        float uvY;
        glm::vec4 color;
    };

    struct UniformBufferObject
    {
        Mat4 model;
        Mat4 view;
        Mat4 proj;
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

    struct GraphicsPushConstants
    {
        Mat4 worldMatrix;
        vk::DeviceAddress vertexBuffer;
    };

    struct GPUSceneData {
        Mat4 view;
        Mat4 proj;
        Mat4 viewProj;
        Mat4 lightSpaceMatrix;  // Light view-projection for shadow mapping
        Vec4 ambientColor;
        Vec4 sunlightDirection; // w for sun power
        Vec4 sunlightColor;
        Vec4 shadowParams;      // x=zNear, y=zFar, z=enablePCF, w=shadowBias
    };

    // Point light for deferred rendering
    struct PointLight {
        Vec4 position;   // xyz = position, w = unused
        Vec4 colorRadius; // xyz = color, w = radius
    };

    // Uniform data for deferred composition pass
    static constexpr int MAX_LIGHTS = 6;
    struct DeferredLightsData {
        PointLight lights[MAX_LIGHTS];
        Vec4 viewPos;    // Camera position for specular
        int numLights;
        int padding[3];  // Align to 16 bytes
    };

    enum class MaterialPass :u8 {
        MainColor,
        Transparent,
        Other
    };

    struct MaterialInstance {
        MaterialPipeline* pipeline;
        vk::DescriptorSet materialSet;
        MaterialPass passType;
    };
} // namespace graphics
