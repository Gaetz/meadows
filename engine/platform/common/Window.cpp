#include "engine/platform/Window.hpp"

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

    // SDL_WINDOW_OPENGL matches the only RHI backend for now; once more
    // backends exist this flag must follow the runtime backend choice (§2.1).
    SDL_Window* sdlWindow = SDL_CreateWindow(
        desc.title.c_str(), desc.width, desc.height,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
    if (!sdlWindow) {
        LOG_ERROR("Window creation failed: {}", SDL_GetError());
        SDL_QuitSubSystem(kSubsystems);
        return nullptr;
    }

    auto window = uptr<Window> { new Window() };
    window->impl->window = sdlWindow;
    window->impl->width = desc.width;
    window->impl->height = desc.height;
    return window;
}

bool Window::pumpEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
            return false;
        case SDL_EVENT_KEY_DOWN:
            // Temporary dev shortcut until a real input system exists.
            if (event.key.key == SDLK_ESCAPE) {
                return false;
            }
            break;
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

} // namespace platform
