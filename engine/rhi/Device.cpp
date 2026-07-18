#include "engine/rhi/Device.hpp"

#include "engine/core/Log.hpp"
#include "engine/rhi/backends/gl/GlDevice.hpp"

#ifdef MEADOWS_RHI_VULKAN
#include "engine/rhi/backends/vulkan/VulkanDevice.hpp"
#endif

namespace rhi {

// Creates exactly the requested backend, or nullptr (logged) if it is not
// compiled in or fails to initialize. The preference/fallback chain (try
// Vulkan, fall back to GL) lives at the call site (Engine::init), because the
// SDL window surface flag is fixed at creation and must match (§2.1).
uptr<Device> Device::create(Backend backend, platform::Window& window) {
    switch (backend) {
    case Backend::OpenGL:
        return createGlDevice(window);
    case Backend::Vulkan:
#ifdef MEADOWS_RHI_VULKAN
        return createVulkanDevice(window);
#else
        LOG_ERROR("Vulkan backend requested but not compiled in "
                  "(MEADOWS_RHI_VULKAN=OFF)");
        return nullptr;
#endif
    }
    LOG_ERROR("Unknown RHI backend");
    return nullptr;
}

} // namespace rhi
