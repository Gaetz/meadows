#pragma once

#include "engine/core/Defines.hpp"

namespace platform {

struct WindowDesc {
    str title { "Meadows" };
    i32 width { 1280 };
    i32 height { 720 };
};

// Application window + OS event pump, SDL3-backed (platform/common/Window.cpp).
// Native/SDL types stay behind the pimpl so this header is platform-clean (§3.1).
class Window {
public:
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Returns nullptr (with a logged error) if video init or creation fails.
    static uptr<Window> create(const WindowDesc& desc);

    // Pumps OS events; returns false once the user requested quit.
    bool pumpEvents();

    i32 width() const;
    i32 height() const;

private:
    Window();

    struct Impl;
    uptr<Impl> impl;
};

} // namespace platform
