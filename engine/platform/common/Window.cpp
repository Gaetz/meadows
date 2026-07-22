#include "engine/platform/Window.hpp"

#ifdef __APPLE__
extern "C" bool meadowsMacosActivate(); // platform/macos/Activation.mm
#endif

#include <SDL3/SDL.h>

#include "engine/core/Log.hpp"

namespace platform {

namespace {
constexpr SDL_InitFlags kSubsystems = SDL_INIT_VIDEO | SDL_INIT_GAMEPAD;
}

struct Window::Impl {
    SDL_Window* window { nullptr };
    i32 width { 0 };
    i32 height { 0 };
    EventHook eventHook;
};

Window::Window() : impl { std::make_unique<Impl>() } {}

Window::~Window() {
    if (impl->window) {
        SDL_DestroyWindow(impl->window);
    }
    SDL_QuitSubSystem(kSubsystems);
}

uptr<Window> Window::create(const WindowDesc& desc) {
    if (!SDL_InitSubSystem(kSubsystems)) {
        LOG_ERROR("SDL init failed: {}", SDL_GetError());
        return nullptr;
    }

    // The surface flag is fixed at creation and must follow the RHI backend
    // chosen at startup (§2.1): OPENGL for the GL backends, VULKAN for the
    // Vulkan backend (+ MoltenVK on macOS).
    const SDL_WindowFlags apiFlag = desc.api == GraphicsApi::Vulkan
                                        ? SDL_WINDOW_VULKAN
                                        : SDL_WINDOW_OPENGL;
    SDL_Window* sdlWindow = SDL_CreateWindow(
        desc.title.c_str(), desc.width, desc.height,
        SDL_WINDOW_RESIZABLE | apiFlag);
    if (!sdlWindow) {
        LOG_ERROR("Window creation failed: {}", SDL_GetError());
        SDL_QuitSubSystem(kSubsystems);
        return nullptr;
    }

    // macOS: an unbundled binary does not become the ACTIVE application by
    // itself, and only the active app's key window receives KEYBOARD events
    // (mouse follows the cursor regardless — which is exactly the confusing
    // symptom: clicks work, keys never arrive; the menu bar keeps showing
    // the launcher). SDL_RaiseWindow is not enough: the activation policy
    // must be set from Cocoa (platform/macos/Activation.mm, §3.1).
    SDL_RaiseWindow(sdlWindow);
#ifdef __APPLE__
    meadowsMacosActivate();
#endif

    auto window = uptr<Window> { new Window() };
    window->impl->window = sdlWindow;
    window->impl->width = desc.width;
    window->impl->height = desc.height;
    return window;
}

bool Window::pumpEvents() {
#ifdef __APPLE__
    // Cooperative activation (see Activation.mm): retry each pump until the
    // app actually becomes active — the pre-runloop attempt is ignored.
    static bool active = false;
    if (!active) {
        active = meadowsMacosActivate();
    }
#endif
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (impl->eventHook) {
            impl->eventHook(&event);
        }
        switch (event.type) {
        case SDL_EVENT_QUIT:
            return false;
        // Escape does not quit: it belongs to the game
        // (pause menu / close screen). Quit = window close or in-game menu.
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            impl->width = event.window.data1;
            impl->height = event.window.data2;
            break;
        default:
            break;
        }
    }
    return true;
}

i32 Window::width() const {
    return impl->width;
}

i32 Window::height() const {
    return impl->height;
}

void Window::setRelativeMouseMode(bool enabled) {
    SDL_SetWindowRelativeMouseMode(impl->window, enabled);
}

void Window::setTextInput(bool enabled) {
    if (enabled) {
        SDL_StartTextInput(impl->window);
    } else {
        SDL_StopTextInput(impl->window);
    }
}

void* Window::nativeHandle() const {
    return impl->window;
}

void Window::setEventHook(EventHook hook) {
    impl->eventHook = std::move(hook);
}

} // namespace platform
