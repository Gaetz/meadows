#include "engine/rhi/backends/gl/GlDevice41.hpp"

#include <glad/gl.h>

#include "engine/core/Log.hpp"
#include "engine/platform/Window.hpp"

// Helper declared in GlDeviceBase.cpp — shared with GlDevice46.
namespace rhi {
u32    glVertexFormatComponents(VertexFormat f);
GLenum glToTopology(PrimitiveTopology t);
}

namespace rhi {

GlDevice41::GlDevice41(uptr<platform::GlContext> context,
                       platform::Window& window,
                       bool baseInstance,
                       GlDeviceBase::PFNBaseInstance pfnBaseInstance)
    : GlDeviceBase(std::move(context), window, baseInstance, pfnBaseInstance) {
}

// --- createBuffer (legacy) ----------------------------------------------------

BufferHandle GlDevice41::createBuffer(const BufferDesc& desc,
                                      const void* initialData) {
    GLenum target;
    switch (desc.usage) {
    case BufferUsage::Index:   target = GL_ELEMENT_ARRAY_BUFFER; break;
    case BufferUsage::Uniform: target = GL_UNIFORM_BUFFER;       break;
    default:                   target = GL_ARRAY_BUFFER;         break;
    }
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(target, buffer);
    glBufferData(target, static_cast<GLsizeiptr>(desc.size), initialData,
                 desc.dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    glBindBuffer(target, 0);
    const u32 id = nextId++;
    buffers.emplace(id, buffer);
    return { id };
}

// --- updateBuffer (legacy) ----------------------------------------------------

void GlDevice41::updateBuffer(BufferHandle handle, const void* data, u64 size,
                              u64 offset) {
    // Buffer objects are not typed; binding to GL_ARRAY_BUFFER is valid for
    // any buffer (including UBOs) for the purpose of glBufferSubData.
    glBindBuffer(GL_ARRAY_BUFFER, buffers.at(handle.id));
    glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(offset),
                    static_cast<GLsizeiptr>(size), data);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// --- createTexture (legacy) ---------------------------------------------------

TextureHandle GlDevice41::createTexture(const TextureDesc& desc,
                                        const void* pixels) {
    GLuint texture = 0;
    const GLint filter =
        desc.filter == FilterMode::Nearest ? GL_NEAREST : GL_LINEAR;

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    // glTexImage2D allocates storage and optionally uploads in one call
    // (unlike the DSA path that separates glTextureStorage2D + SubImage2D).
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 static_cast<GLsizei>(desc.width),
                 static_cast<GLsizei>(desc.height),
                 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    const u32 id = nextId++;
    textures.emplace(id, texture);
    return { id };
}

// --- createPipeline (legacy) --------------------------------------------------

PipelineHandle GlDevice41::createPipeline(const PipelineDesc& desc) {
    const auto shaderIt = shaders.find(desc.shader.id);
    if (shaderIt == shaders.end()) {
        LOG_ERROR("createPipeline: invalid shader handle");
        return {};
    }

    GlPipeline pipeline;
    pipeline.program    = shaderIt->second;
    pipeline.blend      = desc.blend;
    pipeline.glTopology = glToTopology(desc.topology);

    // Generate a bare VAO. Attribute format info is stored for later use in
    // implBindVboSlot, where glVertexAttribPointer bakes buffer + format
    // into the VAO at setVertexBuffer time (GL 4.5+ separates these).
    glGenVertexArrays(1, &pipeline.vao);
    for (u32 slot = 0; slot < desc.vertexBuffers.size(); ++slot) {
        const VertexBufferLayout& layout = desc.vertexBuffers[slot];
        pipeline.strides.push_back(layout.stride);
        pipeline.divisors.push_back(
            layout.stepMode == VertexStepMode::Instance ? 1u : 0u);
        for (const VertexAttribute& attr : layout.attributes) {
            pipeline.attribs41.push_back({
                attr.location,
                static_cast<i32>(glVertexFormatComponents(attr.format)),
                attr.offset,
                slot,
            });
        }
    }

    const u32 id = nextId++;
    pipelines.emplace(id, std::move(pipeline));
    return { id };
}

// --- Command-buffer helpers (legacy) ------------------------------------------

void GlDevice41::implBindTexture(u32 binding, u32 glTexId) {
    glActiveTexture(GL_TEXTURE0 + binding);
    glBindTexture(GL_TEXTURE_2D, glTexId);
}

void GlDevice41::implBindVboSlot(const GlPipeline& p, u32 slot, u32 glBufId) {
    // VAO is bound from GlCommandBuffer::setPipeline. Re-issue
    // glVertexAttribPointer to bake this buffer's binding + format into the VAO.
    glBindBuffer(GL_ARRAY_BUFFER, glBufId);
    const GLsizei stride  = static_cast<GLsizei>(p.strides[slot]);
    const GLuint  divisor = p.divisors[slot];
    for (const auto& attr : p.attribs41) {
        if (attr.slot != slot) {
            continue;
        }
        glVertexAttribPointer(
            attr.location, attr.components, GL_FLOAT, GL_FALSE, stride,
            reinterpret_cast<const void*>(static_cast<uintptr_t>(attr.offset)));
        glEnableVertexAttribArray(attr.location);
        glVertexAttribDivisor(attr.location, divisor);
    }
}

void GlDevice41::implBindEbo(const GlPipeline& /*p*/, u32 glBufId) {
    // VAO is bound from setPipeline; binding GL_ELEMENT_ARRAY_BUFFER while a
    // VAO is active updates that VAO's element buffer binding.
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, glBufId);
}

} // namespace rhi
