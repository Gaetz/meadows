#include "VulkanContext.h"
#include "../BasicServices/Log.h"
#include "Swapchain.h"
#include <set>
#include <cassert>
#include <SDL3/SDL_vulkan.h>

// VMA Implementation
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
#include "VulkanInit.hpp"

using services::Log;

namespace graphics {
    VulkanContext::VulkanContext(SDL_Window *window) : window(window) {
    }

    VulkanContext::~VulkanContext() { cleanup(); }

    void VulkanContext::init() {
        const vkb::Instance vkbInstance = createInstance();
        createSurface();
        const vkb::PhysicalDevice vkbPhysicalDevice = pickPhysicalDevice(vkbInstance);
        createLogicalDevice(vkbPhysicalDevice);
        createAllocator();
        createSwapchain();
        createDescriptorAllocator();
    }

    void VulkanContext::cleanup() {
        // unique_ptr automatically deletes when reset
        swapchain.reset();

        if (allocator) {
            vmaDestroyAllocator(allocator);
            allocator = nullptr;
        }

        instance.destroySurfaceKHR(surface);
        vkb::destroy_debug_utils_messenger(instance, debugMessenger);
        // VkBootstrap handles destruction of instance and device
    }

    vkb::Instance VulkanContext::createInstance() {
        vkb::InstanceBuilder builder;

        // Application info
        builder.set_app_name("Meadows")
                .set_app_version(1, 0, 0)
                .set_engine_name("Meadows")
                .set_engine_version(1, 0, 0)
                .require_api_version(1, 3);

        // Enable validation layers if requested
        if (enableValidationLayers) {
            builder.request_validation_layers();

            // Set custom debug callback
            auto debugCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                    VkDebugUtilsMessageTypeFlagsEXT messageType,
                                    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
                                    void *pUserData) -> VkBool32 {
                Log::Warn("VULKAN VALIDATION | %s ", pCallbackData->pMessage);
                return VK_FALSE;
            };
            builder.set_debug_callback(debugCallback);
        }

        // Get required extensions from SDL
        uint32_t count = 0;
        const char *const *sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&count);
        for (uint32_t i = 0; i < count; ++i) {
            builder.enable_extension(sdlExtensions[i]);
        }

        auto instRet = builder.build();
        if (!instRet) {
            Log::Error("Failed to create Vulkan instance: %s", instRet.error().message().c_str());
            assert(false && "Failed to create Vulkan instance");
        }

        auto vkbInstance = instRet.value();
        instance = vkbInstance.instance;
        debugMessenger = vkbInstance.debug_messenger;
        return vkbInstance;
    }

    void VulkanContext::createSurface() {
        VkSurfaceKHR cSurface;
        bool result = SDL_Vulkan_CreateSurface(window, instance, nullptr, &cSurface);
        assert(result && "failed to create window surface!");
        if (result) {
            surface = cSurface;
        }
    }

    vkb::PhysicalDevice VulkanContext::pickPhysicalDevice(vkb::Instance vkbInstance) {
        // Vulkan 1.3 features
        vk::PhysicalDeviceVulkan13Features features13{};
        features13.sType = vk::StructureType::ePhysicalDeviceVulkan13Features;
        features13.dynamicRendering = true;
        features13.synchronization2 = true;

        // Vulkan 1.2 features
        vk::PhysicalDeviceVulkan12Features features12{};
        features12.sType = vk::StructureType::ePhysicalDeviceVulkan12Features;
        features12.bufferDeviceAddress = true;
        features12.descriptorIndexing = true;

        // Use VkBootstrap to select a GPU
        // We want a GPU that can write to the SDL surface and supports Vulkan 1.3 with the correct features
        vkb::PhysicalDeviceSelector selector{vkbInstance};
        auto physRet = selector
                .set_minimum_version(1, 3)
                .set_required_features_13(features13)
                .set_required_features_12(features12)
                .set_surface(surface)
                .select();

        if (!physRet) {
            Log::Error("Failed to select physical device: %s", physRet.error().message().c_str());
            assert(false && "Failed to select physical device");
        }

        auto vkbPhysicalDevice = physRet.value();
        physicalDevice = vkbPhysicalDevice.physical_device;
        return vkbPhysicalDevice;
    }

    void VulkanContext::createLogicalDevice(vkb::PhysicalDevice vkbPhysicalDevice) {
        // Create the final Vulkan device
        vkb::DeviceBuilder deviceBuilder{vkbPhysicalDevice};

        auto devRet = deviceBuilder.build();
        if (!devRet) {
            Log::Error("Failed to create logical device: %s", devRet.error().message().c_str());
            assert(false && "Failed to create logical device");
        }

        vkb::Device vkbDevice = devRet.value();

        // Get the VkDevice handle used in the rest of a Vulkan application
        device = vkbDevice.device;

        // Get queues
        graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
        presentQueue = vkbDevice.get_queue(vkb::QueueType::present).value();
    }

    void VulkanContext::createAllocator() {
        VmaAllocatorCreateInfo allocatorInfo = {};
        allocatorInfo.physicalDevice = physicalDevice;
        allocatorInfo.device = device;
        allocatorInfo.instance = instance;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;

        vmaCreateAllocator(&allocatorInfo, &allocator);
    }

    void VulkanContext::createSwapchain() {
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        swapchain = std::make_unique<Swapchain>(device, physicalDevice, surface, w, h);
        swapchain->init();

        // Draw image size will match the window
        VkExtent3D drawImageExtent = {
            static_cast<u32>(w),
            static_cast<u32>(h),
            1
        };

        // Hardcoding the draw format to 32 bit float
        drawImage.imageFormat = vk::Format::eR16G16B16A16Sfloat;
        drawImage.imageExtent = drawImageExtent;

        vk::ImageUsageFlags drawImageUsages{};
        drawImageUsages |= vk::ImageUsageFlagBits::eTransferSrc;
        drawImageUsages |= vk::ImageUsageFlagBits::eTransferDst;
        drawImageUsages |= vk::ImageUsageFlagBits::eStorage;
        drawImageUsages |= vk::ImageUsageFlagBits::eColorAttachment;

        vk::ImageCreateInfo renderImageInfo = graphics::imageCreateInfo(drawImage.imageFormat, drawImageUsages,
                                                                        drawImageExtent);

        // For the draw image, we want to allocate it from gpu local memory
        VmaAllocationCreateInfo rimg_allocinfo = {};
        rimg_allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        rimg_allocinfo.requiredFlags = static_cast<VkMemoryPropertyFlags>(vk::MemoryPropertyFlags(
            vk::MemoryPropertyFlagBits::eDeviceLocal));

        // Allocate and create the image
        vmaCreateImage(allocator, reinterpret_cast<const VkImageCreateInfo *>(&renderImageInfo),
                       &rimg_allocinfo, reinterpret_cast<VkImage *>(&drawImage.image), &drawImage.allocation, nullptr);

        // Build a image-view for the draw image to use for rendering
        vk::ImageViewCreateInfo rview_info = graphics::imageViewCreateInfo(
            drawImage.imageFormat, drawImage.image, vk::ImageAspectFlagBits::eColor);
        auto res = device.createImageView(&rview_info, nullptr, &drawImage.imageView);

        mainDeletionQueue.pushFunction([this]() {
            device.destroyImageView(drawImage.imageView, nullptr);
            vmaDestroyImage(allocator, drawImage.image, drawImage.allocation);
        }, "Swapchain's image and view");
    }

    QueueFamilyIndices VulkanContext::findQueueFamilies(vk::PhysicalDevice device) {
        QueueFamilyIndices indices;
        auto queueFamilies = device.getQueueFamilyProperties();

        int i = 0;
        for (const auto &queueFamily: queueFamilies) {
            if (queueFamily.queueFlags & vk::QueueFlagBits::eGraphics) {
                indices.graphicsFamily = i;
            }

            if (device.getSurfaceSupportKHR(i, surface)) {
                indices.presentFamily = i;
            }

            if (indices.isComplete()) {
                break;
            }
            i++;
        }

        return indices;
    }

    bool VulkanContext::checkDeviceExtensionSupport(vk::PhysicalDevice device) {
        auto availableExtensions = device.enumerateDeviceExtensionProperties();
        std::set<str> requiredExtensions(deviceExtensions.begin(),
                                         deviceExtensions.end());

        for (const auto &extension: availableExtensions) {
            requiredExtensions.erase(extension.extensionName);
        }

        return requiredExtensions.empty();
    }

    SwapChainSupportDetails
    VulkanContext::querySwapChainSupport(vk::PhysicalDevice device) {
        SwapChainSupportDetails details;
        details.capabilities = device.getSurfaceCapabilitiesKHR(surface);
        details.formats = device.getSurfaceFormatsKHR(surface);
        details.presentModes = device.getSurfacePresentModesKHR(surface);
        return details;
    }

    void VulkanContext::createDescriptorAllocator() {
        // Create a descriptor pool that will hold 10 sets with 1 image each
        vector<PoolSizeRatio> sizes = {{vk::DescriptorType::eStorageImage, 1.0f}};
        globalDescriptorAllocator = std::make_unique<DescriptorAllocator>(device, 10, sizes);
    }
} // namespace graphics
