#pragma once

#include <vulkan/vulkan.hpp>
#include <cassert>
#include "BasicServices/Log.h"

// Checks a vk::Result and logs + asserts on failure.
// Usage: VK_CHECK(device.createFoo(...));
#define VK_CHECK(expr) \
    do { \
        vk::Result _vkResult = (expr); \
        if (_vkResult != vk::Result::eSuccess) { \
            services::Log::Error("Vulkan error: %s  (at %s:%d)", \
                vk::to_string(_vkResult).c_str(), __FILE__, __LINE__); \
            assert(false && "Vulkan call failed"); \
        } \
    } while(0)
