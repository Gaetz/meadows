#include "VulkanContext.h"
#include "../BasicServices/Log.h"
#include "Swapchain.h"
#include <set>
#include <cassert>
#include <SDL3/SDL_vulkan.h>

// VMA Implementation
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

using services::Log;

namespace graphics {

VulkanContext::VulkanContext(SDL_Window* window) : window(window) {}

VulkanContext::~VulkanContext() { cleanup(); }

void VulkanContext::init() {
  createInstance();
  setupDebugMessenger();
  createSurface();
  pickPhysicalDevice();
  createLogicalDevice();
  createAllocator();
  createSwapchain();
}

void VulkanContext::cleanup() {
  // unique_ptr automatically deletes when reset
  swapchain.reset();

  if (allocator) {
    vmaDestroyAllocator(allocator);
    allocator = nullptr;
  }

  // Vulkan handles can be safely destroyed even if null
  device.destroy();

  if (enableValidationLayers && debugMessenger) {
    auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)instance.getProcAddr(
        "vkDestroyDebugUtilsMessengerEXT");
    if (func != nullptr) {
      func(instance, debugMessenger, nullptr);
    }
  }

  instance.destroySurfaceKHR(surface);
  instance.destroy();
}

void VulkanContext::createInstance() {
  vk::ApplicationInfo appInfo("Vulkan Engine", VK_MAKE_VERSION(1, 0, 0),
                              "No Engine", VK_MAKE_VERSION(1, 0, 0),
                              VK_API_VERSION_1_3);

  auto extensions = getRequiredExtensions();

  vk::InstanceCreateInfo createInfo(
      {}, &appInfo,
      enableValidationLayers ? (uint32_t)validationLayers.size() : 0,
      enableValidationLayers ? validationLayers.data() : nullptr,
      (uint32_t)extensions.size(), extensions.data());

  instance = vk::createInstance(createInfo);
}

void VulkanContext::setupDebugMessenger() {
  if (enableValidationLayers && !checkValidationLayerSupport()) {
    Log::Warn("Validation layers requested but not available! Running without validation.");
    return;
  }

  VkDebugUtilsMessengerCreateInfoEXT createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
  createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  createInfo.pfnUserCallback = debugCallback;

  // Create debug messenger using dynamic dispatch
  auto func = (PFN_vkCreateDebugUtilsMessengerEXT)instance.getProcAddr(
      "vkCreateDebugUtilsMessengerEXT");
  if (func != nullptr) {
    VkDebugUtilsMessengerEXT messenger;
    VkResult result = func(instance, &createInfo, nullptr, &messenger);
    assert(result == VK_SUCCESS && "failed to set up debug messenger!");
    if (result == VK_SUCCESS) {
      debugMessenger = messenger;
    }
  } else {
    assert(false && "failed to set up debug messenger extension!");
  }
}

void VulkanContext::createSurface() {
  VkSurfaceKHR cSurface;
  bool result = SDL_Vulkan_CreateSurface(window, instance, nullptr, &cSurface);
  assert(result && "failed to create window surface!");
  if (result) {
    surface = cSurface;
  }
}

void VulkanContext::pickPhysicalDevice() {
  auto devices = instance.enumeratePhysicalDevices();
  assert(!devices.empty() && "failed to find GPUs with Vulkan support!");

  for (const auto &device : devices) {
    QueueFamilyIndices indices = findQueueFamilies(device);
    bool extensionsSupported = checkDeviceExtensionSupport(device);
    bool swapChainAdequate = false;
    if (extensionsSupported) {
      SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device);
      swapChainAdequate = !swapChainSupport.formats.empty() &&
                          !swapChainSupport.presentModes.empty();
    }

    if (indices.isComplete() && extensionsSupported && swapChainAdequate) {
      physicalDevice = device;
      break;
    }
  }

  assert(physicalDevice && "failed to find a suitable GPU!");
}

void VulkanContext::createLogicalDevice() {
  QueueFamilyIndices indices = findQueueFamilies(physicalDevice);

  std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
  std::set<uint32_t> uniqueQueueFamilies = {indices.graphicsFamily.value(),
                                            indices.presentFamily.value()};

  float queuePriority = 1.0f;
  for (uint32_t queueFamily : uniqueQueueFamilies) {
    queueCreateInfos.push_back(
        vk::DeviceQueueCreateInfo({}, queueFamily, 1, &queuePriority));
  }

  vk::PhysicalDeviceFeatures deviceFeatures;

  vk::DeviceCreateInfo createInfo(
      {}, (uint32_t)queueCreateInfos.size(), queueCreateInfos.data(),
      enableValidationLayers ? (uint32_t)validationLayers.size() : 0,
      enableValidationLayers ? validationLayers.data() : nullptr,
      (uint32_t)deviceExtensions.size(), deviceExtensions.data(),
      &deviceFeatures);

  device = physicalDevice.createDevice(createInfo);

  graphicsQueue = device.getQueue(indices.graphicsFamily.value(), 0);
  presentQueue = device.getQueue(indices.presentFamily.value(), 0);
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
}

QueueFamilyIndices VulkanContext::findQueueFamilies(vk::PhysicalDevice device) {
  QueueFamilyIndices indices;
  auto queueFamilies = device.getQueueFamilyProperties();

  int i = 0;
  for (const auto &queueFamily : queueFamilies) {
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

bool VulkanContext::checkValidationLayerSupport() {
  auto availableLayers = vk::enumerateInstanceLayerProperties();

  for (const char *layerName : validationLayers) {
    bool layerFound = false;
    for (const auto &layerProperties : availableLayers) {
      if (strcmp(layerName, layerProperties.layerName) == 0) {
        layerFound = true;
        break;
      }
    }
    if (!layerFound)
      return false;
  }
  return true;
}

std::vector<const char *> VulkanContext::getRequiredExtensions() {
  uint32_t count = 0;
  const char *const *sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&count);

  std::vector<const char *> extensions(sdlExtensions, sdlExtensions + count);

  if (enableValidationLayers) {
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }

  return extensions;
}

bool VulkanContext::checkDeviceExtensionSupport(vk::PhysicalDevice device) {
  auto availableExtensions = device.enumerateDeviceExtensionProperties();
  std::set<str> requiredExtensions(deviceExtensions.begin(),
                                           deviceExtensions.end());

  for (const auto &extension : availableExtensions) {
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

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanContext::debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
    void *pUserData) {

  Log::Error("validation layer: %s", pCallbackData->pMessage);
  return VK_FALSE;
}

} // namespace graphics
