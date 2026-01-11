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
        Vec4 ambientColor;
        Vec4 sunlightDirection; // w for sun power
        Vec4 sunlightColor;
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

    struct RenderObject {
        u32 indexCount;
        u32 firstIndex;
        vk::Buffer indexBuffer;

        MaterialInstance* material;

        Mat4 transform;
        vk::DeviceAddress vertexBufferAddress;
    };

    struct DrawContext {
        vector<RenderObject> opaqueSurfaces;
        std::vector<RenderObject> transparentSurfaces;
    };

} // namespace graphics
