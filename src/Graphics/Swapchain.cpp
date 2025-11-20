#include "Swapchain.h"
#include <algorithm>
#include <limits>

Swapchain::Swapchain(vk::Device device, vk::PhysicalDevice physicalDevice, vk::SurfaceKHR surface, uint32_t width, uint32_t height)
    : device(device), physicalDevice(physicalDevice), surface(surface), width(width), height(height) {}

Swapchain::~Swapchain() {
    cleanup();
}

void Swapchain::init() {
    createSwapchain();
    createImageViews();
}

void Swapchain::cleanup() {
    for (auto imageView : imageViews) {
        device.destroyImageView(imageView);
    }
    imageViews.clear();

    if (swapchain) {
        device.destroySwapchainKHR(swapchain);
        swapchain = nullptr;
    }
}

void Swapchain::recreate(uint32_t newWidth, uint32_t newHeight) {
    width = newWidth;
    height = newHeight;
    cleanup();
    init();
}

void Swapchain::createSwapchain() {
    SwapChainSupportDetails details;
    details.capabilities = physicalDevice.getSurfaceCapabilitiesKHR(surface);
    details.formats = physicalDevice.getSurfaceFormatsKHR(surface);
    details.presentModes = physicalDevice.getSurfacePresentModesKHR(surface);

    vk::SurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(details.formats);
    vk::PresentModeKHR presentMode = chooseSwapPresentMode(details.presentModes);
    vk::Extent2D extent = chooseSwapExtent(details.capabilities);

    uint32_t imageCount = details.capabilities.minImageCount + 1;
    if (details.capabilities.maxImageCount > 0 && imageCount > details.capabilities.maxImageCount) {
        imageCount = details.capabilities.maxImageCount;
    }

    vk::SwapchainCreateInfoKHR createInfo(
        {},
        surface,
        imageCount,
        surfaceFormat.format,
        surfaceFormat.colorSpace,
        extent,
        1,
        vk::ImageUsageFlagBits::eColorAttachment
    );

    // Assuming graphics and present queues are the same for simplicity now, 
    // or handled by sharing mode if different. 
    // For this basic engine, we assume concurrent sharing if needed or just exclusive.
    // In VulkanContext we picked same queue family if possible or different.
    // Ideally we should pass queue family indices here if they are different.
    // For now, forcing exclusive mode.
    createInfo.imageSharingMode = vk::SharingMode::eExclusive;

    createInfo.preTransform = details.capabilities.currentTransform;
    createInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = nullptr;

    swapchain = device.createSwapchainKHR(createInfo);

    images = device.getSwapchainImagesKHR(swapchain);
    imageFormat = surfaceFormat.format;
    this->extent = extent;
}

void Swapchain::createImageViews() {
    imageViews.resize(images.size());

    for (size_t i = 0; i < images.size(); i++) {
        vk::ImageViewCreateInfo createInfo(
            {},
            images[i],
            vk::ImageViewType::e2D,
            imageFormat,
            vk::ComponentMapping(),
            vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
        );

        imageViews[i] = device.createImageView(createInfo);
    }
}

vk::SurfaceFormatKHR Swapchain::chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& availableFormats) {
    for (const auto& availableFormat : availableFormats) {
        if (availableFormat.format == vk::Format::eB8G8R8A8Srgb && availableFormat.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
            return availableFormat;
        }
    }
    return availableFormats[0];
}

vk::PresentModeKHR Swapchain::chooseSwapPresentMode(const std::vector<vk::PresentModeKHR>& availablePresentModes) {
    for (const auto& availablePresentMode : availablePresentModes) {
        if (availablePresentMode == vk::PresentModeKHR::eMailbox) {
            return availablePresentMode;
        }
    }
    return vk::PresentModeKHR::eFifo;
}

vk::Extent2D Swapchain::chooseSwapExtent(const vk::SurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    } else {
        vk::Extent2D actualExtent = { width, height };
        actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        return actualExtent;
    }
}
