#include "engine/rhi/backends/gl/GlDevice46.hpp"

#include <glad/gl.h>

#include "engine/core/Log.hpp"
#include "engine/platform/Window.hpp"

// Helper declared in GlDeviceBase.cpp — shared with GlDevice41.
namespace rhi {
u32    glVertexFormatComponents(VertexFormat f);
GLenum glToTopology(PrimitiveTopology t);
}

namespace rhi {

GlDevice46::GlDevice46(uptr<platform::GlContext> context,
                       platform::Window& window,
                       bool baseInstance,
                       GlDeviceBase::PFNBaseInstance pfnBaseInstance)
    : GlDeviceBase(std::move(context), window, baseInstance, pfnBaseInstance) {
}

// --- createBuffer (DSA) -------------------------------------------------------

BufferHandle GlDevice46::createBuffer(const BufferDesc& desc,
                                      const void* initialData) {
    GLuint buffer = 0;
    glCreateBuffers(1, &buffer);
    glNamedBufferData(buffer, static_cast<GLsizeiptr>(desc.size), initialData,
                      desc.dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    const u32 id = nextId++;
    buffers.emplace(id, buffer);
    return { id };
}

// --- updateBuffer (DSA) -------------------------------------------------------

void GlDevice46::updateBuffer(BufferHandle handle, const void* data, u64 size,
                              u64 offset) {
    glNamedBufferSubData(buffers.at(handle.id),
                         static_cast<GLintptr>(offset),
                         static_cast<GLsizeiptr>(size), data);
}

// --- createTexture (DSA) ------------------------------------------------------

TextureHandle GlDevice46::createTexture(const TextureDesc& desc,
                                        const void* pixels) {
    GLuint texture = 0;
    const GLint filter =
        desc.filter == FilterMode::Nearest ? GL_NEAREST : GL_LINEAR;

    glCreateTextures(GL_TEXTURE_2D, 1, &texture);
    glTextureStorage2D(texture, 1, GL_RGBA8,
                       static_cast<GLsizei>(desc.width),
                       static_cast<GLsizei>(desc.height));
    if (pixels) {
        glTextureSubImage2D(texture, 0, 0, 0,
                            static_cast<GLsizei>(desc.width),
                            static_cast<GLsizei>(desc.height),
                            GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
    glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, filter);
    glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, filter);
    glTextureParameteri(texture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(texture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    const u32 id = nextId++;
    textures.emplace(id, texture);
    return { id };
}

// --- createPipeline (DSA) -----------------------------------------------------

PipelineHandle GlDevice46::createPipeline(const PipelineDesc& desc) {
    const auto shaderIt = shaders.find(desc.shader.id);
    if (shaderIt == shaders.end()) {
        LOG_ERROR("createPipeline: invalid shader handle");
        return {};
    }

    GlPipeline pipeline;
    pipeline.program        = shaderIt->second;
    pipeline.blend          = desc.blend;
    pipeline.glTopology     = glToTopology(desc.topology);
    pipeline.depth          = desc.depth;
    pipeline.cull           = desc.cull;
    pipeline.depthBias      = desc.depthBias;
    pipeline.depthBiasSlope = desc.depthBiasSlope;
    pipeline.wireframe      = desc.wireframe;

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
                static_cast<GLint>(glVertexFormatComponents(attr.format)),
                GL_FLOAT, GL_FALSE, attr.offset);
            glVertexArrayAttribBinding(pipeline.vao, attr.location, slot);
        }
    }

    const u32 id = nextId++;
    pipelines.emplace(id, std::move(pipeline));
    return { id };
}

// --- Command-buffer helpers (DSA) ---------------------------------------------

void GlDevice46::implBindTexture(u32 binding, u32 glTexId) {
    glBindTextureUnit(binding, glTexId);
}

void GlDevice46::implBindVboSlot(const GlPipeline& p, u32 slot, u32 glBufId) {
    glVertexArrayVertexBuffer(p.vao, slot, glBufId, 0,
                              static_cast<GLsizei>(p.strides[slot]));
}

void GlDevice46::implBindEbo(const GlPipeline& p, u32 glBufId) {
    glVertexArrayElementBuffer(p.vao, glBufId);
}

} // namespace rhi
