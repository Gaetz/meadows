#pragma once

#include <vulkan/vulkan.hpp>
#include <vector>

namespace graphics {

class Swapchain {
public:
    Swapchain(vk::Device device, vk::PhysicalDevice physicalDevice, vk::SurfaceKHR surface, uint32_t width, uint32_t height);
    ~Swapchain();

    void init();
    void cleanup();
    void recreate(uint32_t width, uint32_t height);

    vk::SwapchainKHR getSwapchain() const { return swapchain; }
    vk::Format getImageFormat() const { return imageFormat; }
    vk::Extent2D getExtent() const { return extent; }
    const std::vector<vk::ImageView>& getImageViews() const { return imageViews; }

private:
    void createSwapchain();
    void createImageViews();
    
    vk::SurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& availableFormats);
    vk::PresentModeKHR chooseSwapPresentMode(const std::vector<vk::PresentModeKHR>& availablePresentModes);
    vk::Extent2D chooseSwapExtent(const vk::SurfaceCapabilitiesKHR& capabilities);

    vk::Device device;
    vk::PhysicalDevice physicalDevice;
    vk::SurfaceKHR surface;
    
    vk::SwapchainKHR swapchain;
    std::vector<vk::Image> images;
    std::vector<vk::ImageView> imageViews;
    vk::Format imageFormat;
    vk::Extent2D extent;

    uint32_t width;
    uint32_t height;
};

} // namespace graphics
