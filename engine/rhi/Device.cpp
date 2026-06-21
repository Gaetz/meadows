#include "engine/rhi/Device.hpp"

#include "engine/core/Log.hpp"
#include "engine/rhi/backends/gl/GlDevice.hpp"

namespace rhi {

uptr<Device> Device::create(Backend backend, platform::Window& window) {
    switch (backend) {
    case Backend::OpenGL:
        return createGlDevice(window);
    }
    LOG_ERROR("Unknown RHI backend");
    return nullptr;
}

} // namespace rhi
