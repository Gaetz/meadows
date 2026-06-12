#include "engine/rhi/backends/gl/GlDevice.hpp"

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

void GlCommandBuffer::beginRenderPass(const RenderPassDesc& desc) {
    if (desc.loadOp == LoadOp::Clear) {
        const Color& c = desc.clearColor;
        glClearColor(c.r, c.g, c.b, c.a);
        glClear(GL_COLOR_BUFFER_BIT);
    }
}

void GlCommandBuffer::endRenderPass() {
}

GlDevice::GlDevice(uptr<platform::GlContext> context, platform::Window& window)
    : context { std::move(context) }
    , window { window } {
}

uptr<GlDevice> GlDevice::create(platform::Window& window) {
    auto context = platform::GlContext::create(window, 4, 6);
    if (!context) {
        return nullptr;
    }

    const int version = gladLoadGL(platform::GlContext::getProcAddress);
    if (version == 0) {
        LOG_ERROR("glad failed to load OpenGL functions");
        return nullptr;
    }

    LOG_INFO("OpenGL {}.{} - {} ({})", GLAD_VERSION_MAJOR(version),
             GLAD_VERSION_MINOR(version),
             reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
             reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
    if (!GLAD_GL_ARB_bindless_texture) {
        LOG_WARN("GL_ARB_bindless_texture not available; bindless paths "
                 "will need a fallback");
    }

#ifndef NDEBUG
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(debugCallback, nullptr);
#endif

    return uptr<GlDevice> { new GlDevice(std::move(context), window) };
}

CommandBuffer& GlDevice::beginFrame() {
    glViewport(0, 0, window.width(), window.height());
    return commandBuffer;
}

void GlDevice::endFrame() {
    context->swapBuffers();
}

} // namespace rhi
