#pragma once

#include "engine/core/Defines.hpp"

namespace platform {

class Window;

// The Vulkan half of the platform layer (§3.1): SDL owns the window, so it is
// SDL that knows which instance extensions presenting needs and how to build a
// surface for it. Vulkan handles cross this seam as opaque values so no
// <vulkan/*> type appears in a header — VkInstance is a pointer (void*),
// VkSurfaceKHR is a non-dispatchable handle (u64 covers both 32/64-bit).

// Instance extensions required to present to a window on this platform.
// Empty (with a logged error) if the Vulkan loader is unavailable.
vector<const char*> vulkanInstanceExtensions();

// Creates a presentation surface for `window` on `instance`.
// Returns 0 (with a logged error) on failure.
u64 createVulkanSurface(Window& window, void* instance);

} // namespace platform
