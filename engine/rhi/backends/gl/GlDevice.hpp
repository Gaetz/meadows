#pragma once

// GlDevice is a thin alias for GlDeviceBase so that Device.cpp can call
// GlDevice::create(window) without knowing about the two concrete subclasses.
// The factory selects GlDevice46 (DSA, GL 4.5+) or GlDevice41 (legacy, GL 4.1)
// at runtime based on the actual GL version returned by the driver.

#include "engine/rhi/backends/gl/GlDeviceBase.hpp"

namespace platform { class Window; }

namespace rhi {

using GlDevice = GlDeviceBase;

// Factory: tries GL 4.6, falls back to GL 4.1. Returns nullptr on failure.
uptr<GlDevice> createGlDevice(platform::Window& window);

} // namespace rhi
