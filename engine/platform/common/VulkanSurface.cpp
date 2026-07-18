#include "engine/platform/VulkanSurface.hpp"

// vulkan.h first: SDL_vulkan.h forward-declares the handles it needs but not
// constants like VK_NULL_HANDLE.
#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "engine/core/Log.hpp"
#include "engine/platform/Window.hpp"

namespace platform {

vector<const char*> vulkanInstanceExtensions() {
    u32 count = 0;
    // SDL owns the list (static storage): it must not be freed, and it is only
    // valid once the video subsystem is up — the Window is created first.
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!names) {
        LOG_ERROR("SDL_Vulkan_GetInstanceExtensions failed: {}", SDL_GetError());
        return {};
    }
    return vector<const char*> { names, names + count };
}

u64 createVulkanSurface(Window& window, void* instance) {
    auto* sdlWindow = static_cast<SDL_Window*>(window.nativeHandle());
    if (!sdlWindow || !instance) {
        LOG_ERROR("createVulkanSurface: null window or instance");
        return 0;
    }
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(sdlWindow, static_cast<VkInstance>(instance),
                                  nullptr, &surface)) {
        LOG_ERROR("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError());
        return 0;
    }
    return reinterpret_cast<u64>(surface);
}

} // namespace platform
