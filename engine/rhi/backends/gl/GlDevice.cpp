#include "engine/rhi/backends/gl/GlDevice.hpp"

#include <glad/gl.h>

#include "engine/core/Assert.hpp"
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

// --- GlCommandBuffer ----------------------------------------------------------

void GlCommandBuffer::beginRenderPass(const RenderPassDesc& desc) {
    if (desc.loadOp == LoadOp::Clear) {
        const Color& c = desc.clearColor;
        glClearColor(c.r, c.g, c.b, c.a);
        glClear(GL_COLOR_BUFFER_BIT);
    }
}

void GlCommandBuffer::endRenderPass() {
    currentPipelineId = 0;
}

void GlCommandBuffer::setPipeline(PipelineHandle pipeline) {
    const auto& p = device.pipelines.at(pipeline.id);
    glUseProgram(p.program);
    glBindVertexArray(p.vao);
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
    currentPipelineId = pipeline.id;
}

void GlCommandBuffer::setBindGroup(u32 /*index*/, BindGroupHandle group) {
    const auto& desc = device.bindGroups.at(group.id);
    for (const BindGroupEntry& entry : desc.entries) {
        if (entry.buffer.id != 0) {
            glBindBufferBase(GL_UNIFORM_BUFFER, entry.binding,
                             device.buffers.at(entry.buffer.id));
        } else {
            glBindTextureUnit(entry.binding,
                              device.textures.at(entry.texture.id));
        }
    }
}

void GlCommandBuffer::setVertexBuffer(u32 slot, BufferHandle buffer) {
    ENGINE_ASSERT_MSG(currentPipelineId != 0,
                      "setVertexBuffer requires a bound pipeline");
    const auto& p = device.pipelines.at(currentPipelineId);
    glVertexArrayVertexBuffer(p.vao, slot, device.buffers.at(buffer.id), 0,
                              static_cast<GLsizei>(p.strides[slot]));
}

void GlCommandBuffer::setIndexBuffer(BufferHandle buffer, IndexFormat format) {
    ENGINE_ASSERT_MSG(currentPipelineId != 0,
                      "setIndexBuffer requires a bound pipeline");
    const auto& p = device.pipelines.at(currentPipelineId);
    glVertexArrayElementBuffer(p.vao, device.buffers.at(buffer.id));
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
    glDrawElementsInstancedBaseInstance(
        p.glTopology, static_cast<GLsizei>(indexCount), glIndexType,
        reinterpret_cast<const void*>(offset),
        static_cast<GLsizei>(instanceCount), firstInstance);
}

// --- GlDevice -----------------------------------------------------------------

GlDevice::GlDevice(uptr<platform::GlContext> context, platform::Window& window)
    : context { std::move(context) }
    , window { window }
    , commandBuffer { *this } {
}

GlDevice::~GlDevice() {
    for (auto& [id, pipeline] : pipelines) {
        glDeleteVertexArrays(1, &pipeline.vao);
    }
    for (auto& [id, program] : shaders) {
        glDeleteProgram(program);
    }
    for (auto& [id, texture] : textures) {
        glDeleteTextures(1, &texture);
    }
    for (auto& [id, buffer] : buffers) {
        glDeleteBuffers(1, &buffer);
    }
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

BufferHandle GlDevice::createBuffer(const BufferDesc& desc,
                                    const void* initialData) {
    GLuint buffer = 0;
    glCreateBuffers(1, &buffer);
    glNamedBufferData(buffer, static_cast<GLsizeiptr>(desc.size), initialData,
                      desc.dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    const u32 id = nextId++;
    buffers.emplace(id, buffer);
    return { id };
}

void GlDevice::updateBuffer(BufferHandle handle, const void* data, u64 size,
                            u64 offset) {
    glNamedBufferSubData(buffers.at(handle.id),
                         static_cast<GLintptr>(offset),
                         static_cast<GLsizeiptr>(size), data);
}

void GlDevice::destroyBuffer(BufferHandle handle) {
    if (auto it = buffers.find(handle.id); it != buffers.end()) {
        glDeleteBuffers(1, &it->second);
        buffers.erase(it);
    }
}

TextureHandle GlDevice::createTexture(const TextureDesc& desc,
                                      const void* pixels) {
    GLuint texture = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &texture);
    glTextureStorage2D(texture, 1, GL_RGBA8,
                       static_cast<GLsizei>(desc.width),
                       static_cast<GLsizei>(desc.height));
    if (pixels) {
        glTextureSubImage2D(texture, 0, 0, 0,
                            static_cast<GLsizei>(desc.width),
                            static_cast<GLsizei>(desc.height), GL_RGBA,
                            GL_UNSIGNED_BYTE, pixels);
    }
    const GLint filter =
        desc.filter == FilterMode::Nearest ? GL_NEAREST : GL_LINEAR;
    glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, filter);
    glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, filter);
    glTextureParameteri(texture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(texture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    const u32 id = nextId++;
    textures.emplace(id, texture);
    return { id };
}

void GlDevice::destroyTexture(TextureHandle handle) {
    if (auto it = textures.find(handle.id); it != textures.end()) {
        glDeleteTextures(1, &it->second);
        textures.erase(it);
    }
}

ShaderHandle GlDevice::createShader(const ShaderDesc& desc) {
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

    const u32 id = nextId++;
    shaders.emplace(id, program);
    return { id };
}

void GlDevice::destroyShader(ShaderHandle handle) {
    if (auto it = shaders.find(handle.id); it != shaders.end()) {
        glDeleteProgram(it->second);
        shaders.erase(it);
    }
}

PipelineHandle GlDevice::createPipeline(const PipelineDesc& desc) {
    const auto shaderIt = shaders.find(desc.shader.id);
    if (shaderIt == shaders.end()) {
        LOG_ERROR("createPipeline: invalid shader handle");
        return {};
    }

    GlPipeline pipeline;
    pipeline.program = shaderIt->second;
    pipeline.blend = desc.blend;
    pipeline.glTopology = toGlTopology(desc.topology);

    glCreateVertexArrays(1, &pipeline.vao);
    for (u32 slot = 0; slot < desc.vertexBuffers.size(); ++slot) {
        const VertexBufferLayout& layout = desc.vertexBuffers[slot];
        pipeline.strides.push_back(layout.stride);
        glVertexArrayBindingDivisor(
            pipeline.vao, slot,
            layout.stepMode == VertexStepMode::Instance ? 1 : 0);
        for (const VertexAttribute& attr : layout.attributes) {
            glEnableVertexArrayAttrib(pipeline.vao, attr.location);
            glVertexArrayAttribFormat(
                pipeline.vao, attr.location,
                static_cast<GLint>(vertexFormatComponents(attr.format)),
                GL_FLOAT, GL_FALSE, attr.offset);
            glVertexArrayAttribBinding(pipeline.vao, attr.location, slot);
        }
    }

    const u32 id = nextId++;
    pipelines.emplace(id, std::move(pipeline));
    return { id };
}

void GlDevice::destroyPipeline(PipelineHandle handle) {
    if (auto it = pipelines.find(handle.id); it != pipelines.end()) {
        glDeleteVertexArrays(1, &it->second.vao);
        pipelines.erase(it);
    }
}

BindGroupHandle GlDevice::createBindGroup(const BindGroupDesc& desc) {
    for (const BindGroupEntry& entry : desc.entries) {
        const bool hasBuffer = entry.buffer.id != 0;
        const bool hasTexture = entry.texture.id != 0;
        if (hasBuffer == hasTexture) {
            LOG_ERROR("createBindGroup: entry {} must reference exactly one "
                      "of buffer/texture",
                      entry.binding);
            return {};
        }
    }
    const u32 id = nextId++;
    bindGroups.emplace(id, desc);
    return { id };
}

void GlDevice::destroyBindGroup(BindGroupHandle handle) {
    bindGroups.erase(handle.id);
}

} // namespace rhi
