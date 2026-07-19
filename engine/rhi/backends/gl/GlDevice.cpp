#include "engine/rhi/backends/gl/GlDevice.hpp"
#include "engine/rhi/backends/gl/GlDevice41.hpp"
#include "engine/rhi/backends/gl/GlDevice46.hpp"

#include <glad/gl.h>

#include "engine/core/Log.hpp"
#include "engine/platform/Window.hpp"

namespace rhi {

namespace {

#ifndef NDEBUG
void GLAD_API_PTR debugCallback(GLenum /*source*/, GLenum type, GLuint /*id*/,
                                GLenum severity, GLsizei /*length*/,
                                const GLchar* message, const void* /*user*/) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) {
        return;
    }
    if (type == GL_DEBUG_TYPE_ERROR) {
        LOG_ERROR("GL: {}", message);
    } else {
        LOG_WARN("GL: {}", message);
    }
}
#endif

} // namespace

uptr<GlDevice> createGlDevice(platform::Window& window) {
    // Try GL 4.6 first (full DSA). On macOS and older hardware this fails;
    // we then fall back to GL 4.1 (legacy bind-first paths).
    auto context = platform::GlContext::create(window, 4, 6);
    if (!context) {
        LOG_INFO("GL 4.6 unavailable — trying GL 4.1 (macOS / older hardware)");
        context = platform::GlContext::create(window, 4, 1);
    }
    if (!context) {
        return nullptr;
    }

    const int version = gladLoadGL(platform::GlContext::getProcAddress);
    if (version == 0) {
        LOG_ERROR("glad failed to load OpenGL functions");
        return nullptr;
    }

    const int major = GLAD_VERSION_MAJOR(version);
    const int minor = GLAD_VERSION_MINOR(version);

    // DSA requires GL 4.5 (glCreate*, glNamed*, glVertexArray*, …).
    const bool dsa = (major > 4) || (major == 4 && minor >= 5);

    // glDrawElementsInstancedBaseInstance is GL 4.2 core. Also available as
    // GL_ARB_base_instance on GL 4.1 contexts (common on macOS / older drivers).
    // Load manually so it works regardless of GLAD's compile-time version target.
    platform::GlContext::ProcAddress pfnBaseInst = nullptr;
    if ((major > 4) || (major == 4 && minor >= 2)) {
        pfnBaseInst = platform::GlContext::getProcAddress(
            "glDrawElementsInstancedBaseInstance");
    }
    if (!pfnBaseInst) {
        pfnBaseInst = platform::GlContext::getProcAddress(
            "glDrawElementsInstancedBaseInstanceARB");
    }
    const bool baseInstance = (pfnBaseInst != nullptr);

    LOG_INFO("OpenGL {}.{} — {} ({}) | backend:{} BaseInstance:{}",
             major, minor,
             reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
             reinterpret_cast<const char*>(glGetString(GL_VENDOR)),
             dsa ? "GL46(DSA)" : "GL41(legacy)",
             baseInstance ? "yes" : "no");

    if (!GLAD_GL_ARB_bindless_texture) {
        LOG_DEBUG("GL_ARB_bindless_texture not available (expected on GL < 4.6)");
    }
    if (!baseInstance) {
        LOG_DEBUG("GL_ARB_base_instance unavailable — drawIndexed firstInstance "
                  "must be 0 (SpriteRenderer uses a vertex-buffer offset instead)");
    }

#ifndef NDEBUG
    // GL_KHR_debug / core debug output: GL 4.3+.
    // On GL 4.1 contexts, GL_ARB_debug_output provides the same API under
    // the ARB suffix.
    if (dsa) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(debugCallback, nullptr);
    } else {
        using PFNDebugMessageCallback = void (*)(GLDEBUGPROC, const void*);
        auto pfnDebug = reinterpret_cast<PFNDebugMessageCallback>(
            platform::GlContext::getProcAddress("glDebugMessageCallbackARB"));
        if (pfnDebug) {
            glEnable(0x8242u); // GL_DEBUG_OUTPUT_SYNCHRONOUS_ARB
            pfnDebug(debugCallback, nullptr);
        }
    }
#endif

    if (dsa) {
        return uptr<GlDevice> {
            new GlDevice46(std::move(context), window, baseInstance, pfnBaseInst)
        };
    } else {
        return uptr<GlDevice> {
            new GlDevice41(std::move(context), window, baseInstance, pfnBaseInst)
        };
    }
}

} // namespace rhi
