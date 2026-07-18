#pragma once

#include <functional>

#include "engine/core/Defines.hpp"

namespace platform {

// Which client API the window's surface must be created for. The SDL window
// flag differs (SDL_WINDOW_OPENGL vs SDL_WINDOW_VULKAN) and is fixed at
// creation, so the RHI backend has to be decided before Window::create (§2.1).
// A platform-local enum keeps platform/ free of any rhi:: dependency.
enum class GraphicsApi {
    OpenGL,
    Vulkan,
};

struct WindowDesc {
    str title { "Meadows" };
    i32 width { 1280 };
    i32 height { 720 };
    GraphicsApi api { GraphicsApi::OpenGL };
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

    // Called for every raw platform event during pumpEvents (the pointer is
    // an SDL_Event, kept opaque so this header stays platform-clean). Sole
    // intended consumer: the dev-UI layer. Pass nullptr to remove.
    using EventHook = std::function<void(const void* nativeEvent)>;
    void setEventHook(EventHook hook);

    i32 width() const;
    i32 height() const;

    // Relative mouse mode: hides the cursor and streams unbounded motion
    // deltas (Input::mouseDelta) — mouselook for the 3D fly camera.
    void setRelativeMouseMode(bool enabled);

    // OS text input (IME/UTF-8 typing events for Input::textInput). Enable
    // while a UI text field has focus, disable for gameplay.
    void setTextInput(bool enabled);

    // Opaque native window for platform-internal consumers (GlContext, later
    // a Vulkan surface). Outside platform/, treat it as a token — never cast.
    void* nativeHandle() const;

private:
    Window();

    struct Impl;
    uptr<Impl> impl;
};

} // namespace platform
