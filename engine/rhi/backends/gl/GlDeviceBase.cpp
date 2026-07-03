#include "engine/rhi/backends/gl/GlDeviceBase.hpp"

#include <glad/gl.h>

#include "engine/core/Assert.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Window.hpp"

namespace rhi {

namespace {

u32 vertexFormatComponents(VertexFormat format) {
    switch (format) {
    case VertexFormat::F32x1: return 1;
    case VertexFormat::F32x2: return 2;
    case VertexFormat::F32x3: return 3;
    case VertexFormat::F32x4: return 4;
    }
    return 4;
}

GLenum toGlTopology(PrimitiveTopology topology) {
    switch (topology) {
    case PrimitiveTopology::Triangles:     return GL_TRIANGLES;
    case PrimitiveTopology::TriangleStrip: return GL_TRIANGLE_STRIP;
    case PrimitiveTopology::Lines:         return GL_LINES;
    }
    return GL_TRIANGLES;
}

GLenum toGlCompare(CompareFunc func) {
    switch (func) {
    case CompareFunc::Never:        return GL_NEVER;
    case CompareFunc::Less:         return GL_LESS;
    case CompareFunc::Equal:        return GL_EQUAL;
    case CompareFunc::LessEqual:    return GL_LEQUAL;
    case CompareFunc::Greater:      return GL_GREATER;
    case CompareFunc::NotEqual:     return GL_NOTEQUAL;
    case CompareFunc::GreaterEqual: return GL_GEQUAL;
    case CompareFunc::Always:       return GL_ALWAYS;
    }
    return GL_LESS;
}

GLuint compileStage(GLenum stage, const str& source, const str& debugName) {
    GLuint shader = glCreateShader(stage);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOG_ERROR("Shader '{}' ({}) compile error:\n{}", debugName,
                  stage == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

// Expose helpers to subclass .cpp files without re-declaring them.
u32  glVertexFormatComponents(VertexFormat f) { return vertexFormatComponents(f); }
GLenum glToTopology(PrimitiveTopology t)      { return toGlTopology(t); }
GLenum glToCompare(CompareFunc f)             { return toGlCompare(f); }

// --- GlCommandBuffer ----------------------------------------------------------

void GlCommandBuffer::beginRenderPass(const RenderPassDesc& desc) {
    if (desc.framebuffer.id != 0) {
        const auto& fb = device.framebuffers.at(desc.framebuffer.id);
        glBindFramebuffer(GL_FRAMEBUFFER, fb.name);
        glViewport(0, 0, static_cast<GLsizei>(fb.width),
                   static_cast<GLsizei>(fb.height));
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, device.window.width(), device.window.height());
    }
    GLbitfield clearMask = 0;
    if (desc.loadOp == LoadOp::Clear) {
        const Color& c = desc.clearColor;
        glClearColor(c.r, c.g, c.b, c.a);
        clearMask |= GL_COLOR_BUFFER_BIT;
    }
    if (desc.depthLoadOp == LoadOp::Clear) {
        // A previously bound pipeline may have disabled depth writes, which
        // would silently swallow the clear.
        glDepthMask(GL_TRUE);
        glClearDepthf(desc.clearDepth);
        clearMask |= GL_DEPTH_BUFFER_BIT;
    }
    if (clearMask != 0) {
        glClear(clearMask);
    }
    // State hygiene: a mirrored pass may have flipped the winding.
    glFrontFace(GL_CCW);
}

void GlCommandBuffer::endRenderPass() {
    // Defensive: whatever pass just ran, ImGui and the next pass start from
    // the backbuffer.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    currentPipelineId = 0;
}

void GlCommandBuffer::setViewport(u32 x, u32 y, u32 width, u32 height) {
    glViewport(static_cast<GLint>(x), static_cast<GLint>(y),
               static_cast<GLsizei>(width), static_cast<GLsizei>(height));
}

void GlCommandBuffer::setFrontFace(FrontFace frontFace) {
    glFrontFace(frontFace == FrontFace::Clockwise ? GL_CW : GL_CCW);
}

void GlCommandBuffer::setPipeline(PipelineHandle pipeline) {
    const auto& p = device.pipelines.at(pipeline.id);
    glUseProgram(p.program);
    glBindVertexArray(p.vao);
    // Every piece of state is applied — enables AND disables — so a pipeline
    // never inherits state from the previous one (anti-leak guarantee: the
    // sprite pipeline after a 3D pass turns depth/cull back off).
    switch (p.blend) {
    case BlendMode::Opaque:
        glDisable(GL_BLEND);
        break;
    case BlendMode::Alpha:
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case BlendMode::Additive:
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        break;
    }
    if (p.depth.testEnable) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(toGlCompare(p.depth.compare));
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(p.depth.writeEnable ? GL_TRUE : GL_FALSE);
    switch (p.cull) {
    case CullMode::None:
        glDisable(GL_CULL_FACE);
        break;
    case CullMode::Back:
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        break;
    case CullMode::Front:
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        break;
    }
    if (p.depthBias != 0.0f || p.depthBiasSlope != 0.0f) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(p.depthBiasSlope, p.depthBias);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    glPolygonMode(GL_FRONT_AND_BACK, p.wireframe ? GL_LINE : GL_FILL);
    currentPipelineId = pipeline.id;
}

void GlCommandBuffer::setBindGroup(u32 /*index*/, BindGroupHandle group) {
    const auto& desc = device.bindGroups.at(group.id);
    for (const BindGroupEntry& entry : desc.entries) {
        if (entry.buffer.id != 0) {
            glBindBufferBase(GL_UNIFORM_BUFFER, entry.binding,
                             device.buffers.at(entry.buffer.id));
        } else {
            device.implBindTexture(entry.binding,
                                   device.textures.at(entry.texture.id).name);
            // Explicit unbind when no sampler: the texture's own parameters
            // apply (legacy 2D bind groups keep working after a 3D pass
            // bound a sampler on the same unit).
            glBindSampler(entry.binding,
                          entry.sampler.id != 0
                              ? device.samplers.at(entry.sampler.id)
                              : 0);
        }
    }
}

void GlCommandBuffer::setVertexBuffer(u32 slot, BufferHandle buffer) {
    ENGINE_ASSERT_MSG(currentPipelineId != 0,
                      "setVertexBuffer requires a bound pipeline");
    const auto& p = device.pipelines.at(currentPipelineId);
    device.implBindVboSlot(p, slot, device.buffers.at(buffer.id));
}

void GlCommandBuffer::setIndexBuffer(BufferHandle buffer, IndexFormat format) {
    ENGINE_ASSERT_MSG(currentPipelineId != 0,
                      "setIndexBuffer requires a bound pipeline");
    const auto& p = device.pipelines.at(currentPipelineId);
    device.implBindEbo(p, device.buffers.at(buffer.id));
    glIndexType = format == IndexFormat::U16 ? GL_UNSIGNED_SHORT
                                             : GL_UNSIGNED_INT;
    indexByteSize = format == IndexFormat::U16 ? 2 : 4;
}

void GlCommandBuffer::draw(u32 vertexCount, u32 instanceCount,
                           u32 firstVertex) {
    const auto& p = device.pipelines.at(currentPipelineId);
    glDrawArraysInstanced(p.glTopology, static_cast<GLint>(firstVertex),
                          static_cast<GLsizei>(vertexCount),
                          static_cast<GLsizei>(instanceCount));
}

void GlCommandBuffer::drawIndexed(u32 indexCount, u32 instanceCount,
                                  u32 firstIndex, u32 firstInstance) {
    const auto& p = device.pipelines.at(currentPipelineId);
    const auto offset =
        static_cast<size_t>(firstIndex) * static_cast<size_t>(indexByteSize);
    if (device.baseInstance_) {
        using PFN = void (*)(GLenum, GLsizei, GLenum, const void*, GLsizei, GLuint);
        reinterpret_cast<PFN>(device.pfnDrawElementsInstancedBaseInstance_)(
            p.glTopology, static_cast<GLsizei>(indexCount), glIndexType,
            reinterpret_cast<const void*>(offset),
            static_cast<GLsizei>(instanceCount), firstInstance);
    } else {
        ENGINE_ASSERT_MSG(firstInstance == 0,
                          "drawIndexed: non-zero firstInstance requires GL 4.2+ "
                          "or GL_ARB_base_instance — not available on this GPU");
        glDrawElementsInstanced(
            p.glTopology, static_cast<GLsizei>(indexCount), glIndexType,
            reinterpret_cast<const void*>(offset),
            static_cast<GLsizei>(instanceCount));
    }
}

void GlCommandBuffer::copyTexture(TextureHandle src, TextureHandle dst) {
    if (!device.caps().copyTexture) {
        LOG_ERROR("copyTexture: not supported by this backend (check "
                  "Device::caps().copyTexture)");
        return;
    }
    const auto& srcTex = device.textures.at(src.id);
    const auto& dstTex = device.textures.at(dst.id);
    glCopyImageSubData(srcTex.name, GL_TEXTURE_2D, 0, 0, 0, 0, dstTex.name,
                       GL_TEXTURE_2D, 0, 0, 0, 0,
                       static_cast<GLsizei>(srcTex.width),
                       static_cast<GLsizei>(srcTex.height), 1);
}

// --- GlDeviceBase -------------------------------------------------------------

GlDeviceBase::GlDeviceBase(uptr<platform::GlContext> context,
                           platform::Window& window,
                           bool baseInstance,
                           PFNBaseInstance pfnBaseInstance)
    : context { std::move(context) }
    , window { window }
    , commandBuffer { *this }
    , baseInstance_ { baseInstance }
    , pfnDrawElementsInstancedBaseInstance_ { pfnBaseInstance } {
}

GlDeviceBase::~GlDeviceBase() {
    for (auto& [id, framebuffer] : framebuffers) {
        glDeleteFramebuffers(1, &framebuffer.name);
    }
    for (auto& [id, sampler] : samplers) {
        glDeleteSamplers(1, &sampler);
    }
    for (auto& [id, pipeline] : pipelines) {
        glDeleteVertexArrays(1, &pipeline.vao);
    }
    for (auto& [id, program] : shaders) {
        glDeleteProgram(program);
    }
    for (auto& [id, texture] : textures) {
        glDeleteTextures(1, &texture.name);
    }
    for (auto& [id, buffer] : buffers) {
        glDeleteBuffers(1, &buffer);
    }
}

CommandBuffer& GlDeviceBase::beginFrame() {
    // Viewport is per render pass (beginRenderPass sets it from its target).
    return commandBuffer;
}

void GlDeviceBase::endFrame() {
    context->swapBuffers();
}

void GlDeviceBase::destroyBuffer(BufferHandle handle) {
    if (auto it = buffers.find(handle.id); it != buffers.end()) {
        glDeleteBuffers(1, &it->second);
        buffers.erase(it);
    }
}

void GlDeviceBase::destroyTexture(TextureHandle handle) {
    if (auto it = textures.find(handle.id); it != textures.end()) {
        glDeleteTextures(1, &it->second.name);
        textures.erase(it);
    }
}

void GlDeviceBase::destroySampler(SamplerHandle handle) {
    if (auto it = samplers.find(handle.id); it != samplers.end()) {
        glDeleteSamplers(1, &it->second);
        samplers.erase(it);
    }
}

void GlDeviceBase::destroyFramebuffer(FramebufferHandle handle) {
    if (auto it = framebuffers.find(handle.id); it != framebuffers.end()) {
        glDeleteFramebuffers(1, &it->second.name);
        framebuffers.erase(it);
    }
}

// 3D-path features: real implementations live in GlDevice46. These logged
// stubs are what the GL 4.1 backend reports until a degraded mode promotes
// them (callers must check Device::caps() first).

void GlDeviceBase::generateMipmaps(TextureHandle /*handle*/) {
    LOG_ERROR("generateMipmaps: not supported by this backend (check "
              "Device::caps().mipmapGeneration)");
}

SamplerHandle GlDeviceBase::createSampler(const SamplerDesc& /*desc*/) {
    LOG_ERROR("createSampler: not supported by this backend (check "
              "Device::caps().samplerObjects)");
    return {};
}

FramebufferHandle
GlDeviceBase::createFramebuffer(const FramebufferDesc& /*desc*/) {
    LOG_ERROR("createFramebuffer: not supported by this backend (check "
              "Device::caps().offscreenTargets)");
    return {};
}

ShaderHandle GlDeviceBase::createShader(const ShaderDesc& desc) {
    const GLuint vertex =
        compileStage(GL_VERTEX_SHADER, desc.vertexSource, desc.debugName);
    if (vertex == 0) {
        return {};
    }
    const GLuint fragment =
        compileStage(GL_FRAGMENT_SHADER, desc.fragmentSource, desc.debugName);
    if (fragment == 0) {
        glDeleteShader(vertex);
        return {};
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOG_ERROR("Shader '{}' link error:\n{}", desc.debugName, log);
        glDeleteProgram(program);
        return {};
    }

    // Explicit binding setup via API — required on GL 4.1 (GLSL 4.10 lacks
    // layout(binding=N) for UBOs and samplers); harmless on GL 4.6 (overrides
    // the layout qualifier with the same value).
    for (const auto& [name, binding] : desc.uniformBlocks) {
        const GLuint idx = glGetUniformBlockIndex(program, name.c_str());
        if (idx != GL_INVALID_INDEX) {
            glUniformBlockBinding(program, idx, binding);
        }
    }
    if (!desc.samplers.empty()) {
        glUseProgram(program);
        for (const auto& [name, unit] : desc.samplers) {
            const GLint loc = glGetUniformLocation(program, name.c_str());
            if (loc >= 0) {
                glUniform1i(loc, static_cast<GLint>(unit));
            }
        }
        glUseProgram(0);
    }

    const u32 id = nextId++;
    shaders.emplace(id, program);
    return { id };
}

void GlDeviceBase::destroyShader(ShaderHandle handle) {
    if (auto it = shaders.find(handle.id); it != shaders.end()) {
        glDeleteProgram(it->second);
        shaders.erase(it);
    }
}

void GlDeviceBase::destroyPipeline(PipelineHandle handle) {
    if (auto it = pipelines.find(handle.id); it != pipelines.end()) {
        glDeleteVertexArrays(1, &it->second.vao);
        pipelines.erase(it);
    }
}

BindGroupHandle GlDeviceBase::createBindGroup(const BindGroupDesc& desc) {
    for (const BindGroupEntry& entry : desc.entries) {
        const bool hasBuffer  = entry.buffer.id != 0;
        const bool hasTexture = entry.texture.id != 0;
        if (hasBuffer == hasTexture) {
            LOG_ERROR("createBindGroup: entry {} must reference exactly one "
                      "of buffer/texture",
                      entry.binding);
            return {};
        }
        if (entry.sampler.id != 0 && !hasTexture) {
            LOG_ERROR("createBindGroup: entry {} has a sampler without a "
                      "texture",
                      entry.binding);
            return {};
        }
    }
    const u32 id = nextId++;
    bindGroups.emplace(id, desc);
    return { id };
}

void GlDeviceBase::destroyBindGroup(BindGroupHandle handle) {
    bindGroups.erase(handle.id);
}

} // namespace rhi
