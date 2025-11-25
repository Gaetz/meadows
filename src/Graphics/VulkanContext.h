#pragma once

#include "Types.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

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
  class Swapchain *getSwapchain() const { return swapchain; }
  SDL_Window *getWindow() const { return window; }

  QueueFamilyIndices findQueueFamilies(vk::PhysicalDevice device);

private:
  void createInstance();
  void setupDebugMessenger();

  static VKAPI_ATTR VkBool32 VKAPI_CALL
  debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                VkDebugUtilsMessageTypeFlagsEXT messageType,
                const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
                void *pUserData);

  void createSurface();
  void pickPhysicalDevice();
  void createLogicalDevice();
  void createAllocator();
  void createSwapchain();

  bool checkValidationLayerSupport();
  std::vector<const char *> getRequiredExtensions();
  bool checkDeviceExtensionSupport(vk::PhysicalDevice device);
  SwapChainSupportDetails querySwapChainSupport(vk::PhysicalDevice device);

  SDL_Window *window;

  vk::Instance instance;
  vk::DebugUtilsMessengerEXT debugMessenger;
  vk::SurfaceKHR surface;
  vk::PhysicalDevice physicalDevice;
  vk::Device device;
  vk::Queue graphicsQueue;
  vk::Queue presentQueue;

  VmaAllocator allocator;
  class Swapchain *swapchain{nullptr};

  const std::vector<const char *> validationLayers = {
      "VK_LAYER_KHRONOS_validation"};

  const std::vector<const char *> deviceExtensions = {
      VK_KHR_SWAPCHAIN_EXTENSION_NAME};

#ifdef NDEBUG
  bool enableValidationLayers = false;
#else
  bool enableValidationLayers = true;
#endif
};
