#include "Swapchain.h"
#include <algorithm>
#include <limits>
#include "Types.h"
#include "Defines.h"

namespace graphics {

Swapchain::Swapchain(vk::Device device, vk::PhysicalDevice physicalDevice, vk::SurfaceKHR surface, uint32_t width, uint32_t height)
    : device(device), physicalDevice(physicalDevice), surface(surface), width(width), height(height) {}

Swapchain::~Swapchain() {
    cleanup();
}

void Swapchain::init() {
    createSwapchain(width, height);
}

void Swapchain::cleanup() {
    for (auto imageView : swapchainImageViews) {
        device.destroyImageView(imageView);
    }
    swapchainImageViews.clear();

    if (swapchain) {
        device.destroySwapchainKHR(swapchain);
        swapchain = nullptr;
    }
}

void Swapchain::recreate(uint32_t newWidth, uint32_t newHeight) {
    width = newWidth;
    height = newHeight;

    vk::SwapchainKHR oldSwapchain = swapchain;

    // Destroy old image views
    for (auto imageView : swapchainImageViews) {
        device.destroyImageView(imageView);
    }
    swapchainImageViews.clear();

    // Create new swapchain using old one
    createSwapchain(newWidth, newHeight, oldSwapchain);

    // Destroy old swapchain
    if (oldSwapchain) {
        device.destroySwapchainKHR(oldSwapchain);
    }
}

void Swapchain::createSwapchain(uint32_t width, uint32_t height, vk::SwapchainKHR oldSwapchain) {
    vkb::SwapchainBuilder swapchainBuilder{ physicalDevice, device, surface };

    imageFormat = vk::Format::eB8G8R8A8Unorm;

    vkb::Swapchain vkbSwapchain = swapchainBuilder
        .set_desired_format(vk::SurfaceFormatKHR{ imageFormat, vk::ColorSpaceKHR::eSrgbNonlinear })
        .set_desired_present_mode((VkPresentModeKHR)vk::PresentModeKHR::eFifo)
        .set_desired_extent(width, height)
        .add_image_usage_flags((VkImageUsageFlags)vk::ImageUsageFlagBits::eTransferDst)
        .set_old_swapchain(oldSwapchain)
        .build()
        .value();

    extent = vkbSwapchain.extent;
    swapchain = vkbSwapchain.swapchain;
    std::vector<VkImage> images = vkbSwapchain.get_images().value();
    swapchainImages.assign(images.begin(), images.end());
    std::vector<VkImageView> imageViews = vkbSwapchain.get_image_views().value();
    swapchainImageViews.assign(imageViews.begin(), imageViews.end());
}

/*
void Swapchain::createImageViews() {
    swapchainImageViews.resize(swapchainImages.size());

    for (size_t i = 0; i < swapchainImages.size(); i++) {
        vk::ImageViewCreateInfo createInfo(
            {},
            swapchainImages[i],
            vk::ImageViewType::e2D,
            imageFormat,
            vk::ComponentMapping(),
            vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
        );

        swapchainImageViews[i] = device.createImageView(createInfo);
    }
}
*/

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

} // namespace graphics
