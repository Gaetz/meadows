#pragma once

#include "engine/core/Defines.hpp"

namespace platform {

class Window;

// OpenGL context bound to a Window. Lives in the platform layer so that no
// code outside platform/ touches SDL for context management (§3.1). Consumed
// only by the GL RHI backend.
class GlContext {
public:
    ~GlContext();
    GlContext(const GlContext&) = delete;
    GlContext& operator=(const GlContext&) = delete;

    // Creates a core-profile context (debug context in debug builds) and
    // makes it current. Returns nullptr (with a logged error) on failure.
    static uptr<GlContext> create(Window& window, i32 major, i32 minor);

    void swapBuffers();
    void setVsync(bool enabled);

    // Loader callback for glad: resolves a GL function by name.
    using ProcAddress = void (*)();
    static ProcAddress getProcAddress(const char* name);

private:
    GlContext();

    struct Impl;
    uptr<Impl> impl;
};

} // namespace platform
