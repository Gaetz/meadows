#include "engine/platform/GlContext.hpp"

#include <SDL3/SDL.h>

#include "engine/core/Log.hpp"
#include "engine/platform/Window.hpp"

namespace platform {

struct GlContext::Impl {
    SDL_Window* window { nullptr };
    SDL_GLContext context { nullptr };
};

GlContext::GlContext() : impl { std::make_unique<Impl>() } {}

GlContext::~GlContext() {
    if (impl->context) {
        SDL_GL_DestroyContext(impl->context);
    }
}

uptr<GlContext> GlContext::create(Window& window, i32 major, i32 minor) {
    auto* sdlWindow = static_cast<SDL_Window*>(window.nativeHandle());

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    // The 3D path depth-tests against the backbuffer; SDL's default depth
    // size is not guaranteed to be more than 16 bits.
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
#ifndef NDEBUG
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
#endif

    SDL_GLContext sdlContext = SDL_GL_CreateContext(sdlWindow);
    if (!sdlContext) {
        LOG_WARN("GL {}.{} context creation failed: {}", major, minor,
                 SDL_GetError());
        return nullptr;
    }

    auto context = uptr<GlContext> { new GlContext() };
    context->impl->window = sdlWindow;
    context->impl->context = sdlContext;
    context->setVsync(true);
    return context;
}

void GlContext::swapBuffers() {
    SDL_GL_SwapWindow(impl->window);
}

void GlContext::setVsync(bool enabled) {
    SDL_GL_SetSwapInterval(enabled ? 1 : 0);
}

GlContext::ProcAddress GlContext::getProcAddress(const char* name) {
    return SDL_GL_GetProcAddress(name);
}

} // namespace platform
