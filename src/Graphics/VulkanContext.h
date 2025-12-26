#pragma once

#include "../Defines.h"
#include "Types.h"
#include <SDL3/SDL.h>
#include <VkBootstrap.h>

#include "DeletionQueue.hpp"
#include "DescriptorAllocator.h"

namespace graphics {
    class Swapchain;

    class VulkanContext {
    public:
        VulkanContext(SDL_Window *window);

        ~VulkanContext();

        void init();

        void cleanup();

        vk::Instance getInstance() const { return instance; }
        vk::PhysicalDevice getPhysicalDevice() const { return physicalDevice; }
        vk::Device getDevice() const { return device; }
        vk::Queue getGraphicsQueue() const { return graphicsQueue; }
        vk::Queue getPresentQueue() const { return presentQueue; }
        vk::SurfaceKHR getSurface() const { return surface; }
        VmaAllocator getAllocator() const { return allocator; }
        Swapchain *getSwapchain() const { return swapchain.get(); }
        SDL_Window *getWindow() const { return window; }
        DescriptorAllocator* getGlobalDescriptorAllocator() const { return globalDescriptorAllocator.get(); }

        AllocatedImage &getDrawImage() { return drawImage; }

        QueueFamilyIndices findQueueFamilies(vk::PhysicalDevice device);

        void addToMainDeletionQueue(std::function<void()> &&function, str&& name) {
            mainDeletionQueue.pushFunction(std::move(function), std::move(name));
        }
        void flushMainDeletionQueue() { mainDeletionQueue.flush(); }

    private:
        vkb::Instance createInstance();

        void createSurface();

        vkb::PhysicalDevice pickPhysicalDevice(vkb::Instance vkbInstance);

        void createLogicalDevice(vkb::PhysicalDevice vkbPhysicalDevice);

        void createAllocator();

        void createSwapchain();

        bool checkDeviceExtensionSupport(vk::PhysicalDevice device);

        SwapChainSupportDetails querySwapChainSupport(vk::PhysicalDevice device);

        void createDescriptorAllocator();

        SDL_Window *window;

        vk::Instance instance;
        vk::DebugUtilsMessengerEXT debugMessenger;
        vk::SurfaceKHR surface;
        vk::PhysicalDevice physicalDevice;
        vk::Device device;
        vk::Queue graphicsQueue;
        vk::Queue presentQueue;
        DeletionQueue mainDeletionQueue;

        VmaAllocator allocator;
        uptr<Swapchain> swapchain{nullptr};
        uptr<DescriptorAllocator> globalDescriptorAllocator{nullptr};

        AllocatedImage drawImage;

        const std::vector<const char *> validationLayers = {
            "VK_LAYER_KHRONOS_validation"
        };

        const std::vector<const char *> deviceExtensions = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME
        };

#ifdef NDEBUG
        bool enableValidationLayers = false;
#else
        bool enableValidationLayers = true;
#endif
    };
} // namespace graphics
