#include "engine/rhi/backends/gl/GlDevice46.hpp"

#include <algorithm>

#include <glad/gl.h>

#include "engine/core/Log.hpp"
#include "engine/platform/Window.hpp"
#include "engine/rhi/backends/gl/GlConvert.hpp"

namespace rhi {

GlDevice46::GlDevice46(uptr<platform::GlContext> context,
                       platform::Window& window,
                       bool baseInstance,
                       GlDeviceBase::PFNBaseInstance pfnBaseInstance)
    : GlDeviceBase(std::move(context), window, baseInstance, pfnBaseInstance) {
    caps_ = { .offscreenTargets = true,
              .textureArrays = true,
              .hdrFormats = true,
              .samplerObjects = true,
              .mipmapGeneration = true,
              .copyTexture = true,      // glCopyImageSubData (GL 4.3+)
              .computeShaders = true,   // glDispatchCompute (GL 4.3+)
              .timerQueries = true,     // GL_TIMESTAMP queries (GL 3.3+)
              .volumeTextures = true }; // GL_TEXTURE_3D (GI, chantier RC)
}

namespace {

GLenum toGlInternalFormat(TextureFormat format) {
    switch (format) {
    case TextureFormat::RGBA8:    return GL_RGBA8;
    case TextureFormat::SRGBA8:   return GL_SRGB8_ALPHA8;
    case TextureFormat::RGBA16F:  return GL_RGBA16F;
    case TextureFormat::R16F:     return GL_R16F;
    case TextureFormat::R32F:     return GL_R32F;
    case TextureFormat::Depth32F: return GL_DEPTH_COMPONENT32F;
    }
    return GL_RGBA8;
}

bool acceptsPixelUpload(TextureFormat format) {
    // R16F uploads take tightly packed f32 texels (GL converts on upload).
    return format == TextureFormat::RGBA8 ||
           format == TextureFormat::SRGBA8 ||
           format == TextureFormat::R16F;
}

void uploadFormatFor(TextureFormat format, GLenum& pixelFormat,
                     GLenum& pixelType) {
    if (format == TextureFormat::R16F) {
        pixelFormat = GL_RED;
        pixelType = GL_FLOAT;
    } else {
        pixelFormat = GL_RGBA;
        pixelType = GL_UNSIGNED_BYTE;
    }
}

GLint toGlWrap(AddressMode mode) {
    return mode == AddressMode::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
}

GLint toGlMinFilter(FilterMode filter, bool mipmapFilter) {
    if (filter == FilterMode::Nearest) {
        return mipmapFilter ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
    }
    return mipmapFilter ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
}

} // namespace

// --- createBuffer (DSA) -------------------------------------------------------

BufferHandle GlDevice46::createBuffer(const BufferDesc& desc,
                                      const void* initialData) {
    GLuint buffer = 0;
    glCreateBuffers(1, &buffer);
    glNamedBufferData(buffer, static_cast<GLsizeiptr>(desc.size), initialData,
                      desc.readback ? GL_STREAM_READ
                      : desc.dynamic ? GL_DYNAMIC_DRAW
                                     : GL_STATIC_DRAW);
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
    if (pixels && !acceptsPixelUpload(desc.format)) {
        LOG_ERROR("createTexture: initial pixels only supported for "
                  "RGBA8/SRGBA8 (render-target formats are created empty)");
        return {};
    }

    const bool isArray = desc.arrayLayers > 1;
    const bool isVolume = desc.depth > 1; // G0: 3D textures (GI clipmap)
    if (isVolume && isArray) {
        LOG_ERROR("createTexture: depth and arrayLayers are exclusive");
        return {};
    }
    if (isVolume && pixels) {
        LOG_ERROR("createTexture: volumes are GPU-written (storageImage), "
                  "initial pixels unsupported");
        return {};
    }
    const GLenum internal = toGlInternalFormat(desc.format);
    GLenum pixelFormat = GL_RGBA;
    GLenum pixelType = GL_UNSIGNED_BYTE;
    uploadFormatFor(desc.format, pixelFormat, pixelType);
    GLuint texture = 0;
    glCreateTextures(isVolume  ? GL_TEXTURE_3D
                     : isArray ? GL_TEXTURE_2D_ARRAY
                               : GL_TEXTURE_2D,
                     1, &texture);
    if (isVolume) {
        glTextureStorage3D(texture, static_cast<GLsizei>(desc.mipLevels),
                           internal, static_cast<GLsizei>(desc.width),
                           static_cast<GLsizei>(desc.height),
                           static_cast<GLsizei>(desc.depth));
        glTextureParameteri(texture, GL_TEXTURE_WRAP_R, toGlWrap(desc.wrap));
    } else if (isArray) {
        glTextureStorage3D(texture, static_cast<GLsizei>(desc.mipLevels),
                           internal, static_cast<GLsizei>(desc.width),
                           static_cast<GLsizei>(desc.height),
                           static_cast<GLsizei>(desc.arrayLayers));
        if (pixels) {
            glTextureSubImage3D(texture, 0, 0, 0, 0,
                                static_cast<GLsizei>(desc.width),
                                static_cast<GLsizei>(desc.height),
                                static_cast<GLsizei>(desc.arrayLayers),
                                pixelFormat, pixelType, pixels);
        }
    } else {
        glTextureStorage2D(texture, static_cast<GLsizei>(desc.mipLevels),
                           internal, static_cast<GLsizei>(desc.width),
                           static_cast<GLsizei>(desc.height));
        if (pixels) {
            glTextureSubImage2D(texture, 0, 0, 0,
                                static_cast<GLsizei>(desc.width),
                                static_cast<GLsizei>(desc.height),
                                pixelFormat, pixelType, pixels);
        }
    }

    // Creation-time parameters serve textures used WITHOUT a sampler object
    // (the legacy 2D path); a bound SamplerHandle overrides all of this.
    glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER,
                        toGlMinFilter(desc.filter, desc.mipLevels > 1));
    glTextureParameteri(
        texture, GL_TEXTURE_MAG_FILTER,
        desc.filter == FilterMode::Nearest ? GL_NEAREST : GL_LINEAR);
    glTextureParameteri(texture, GL_TEXTURE_WRAP_S, toGlWrap(desc.wrap));
    glTextureParameteri(texture, GL_TEXTURE_WRAP_T, toGlWrap(desc.wrap));

    const u32 id = nextId++;
    textures.emplace(id, GlTexture { .name = texture,
                                     .width = desc.width,
                                     .height = desc.height,
                                     .arrayLayers = desc.arrayLayers,
                                     .depth = desc.depth,
                                     .format = desc.format });
    return { id };
}

void GlDevice46::generateMipmaps(TextureHandle handle) {
    glGenerateTextureMipmap(textures.at(handle.id).name);
}

SamplerHandle GlDevice46::createSampler(const SamplerDesc& desc) {
    GLuint sampler = 0;
    glCreateSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER,
                        toGlMinFilter(desc.minFilter, desc.mipmapFilter));
    glSamplerParameteri(
        sampler, GL_TEXTURE_MAG_FILTER,
        desc.magFilter == FilterMode::Nearest ? GL_NEAREST : GL_LINEAR);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, toGlWrap(desc.addressU));
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, toGlWrap(desc.addressV));
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, toGlWrap(desc.addressW));
    if (desc.compare != CompareFunc::Never) {
        glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_MODE,
                            GL_COMPARE_REF_TO_TEXTURE);
        glSamplerParameteri(sampler, GL_TEXTURE_COMPARE_FUNC,
                            static_cast<GLint>(glToCompare(desc.compare)));
    }
    if (desc.maxAnisotropy > 1.0f) {
        glSamplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY,
                            desc.maxAnisotropy);
    }
    const u32 id = nextId++;
    samplers.emplace(id, sampler);
    return { id };
}

FramebufferHandle GlDevice46::createFramebuffer(const FramebufferDesc& desc) {
    GLuint framebuffer = 0;
    glCreateFramebuffers(1, &framebuffer);

    u32 width = 0;
    u32 height = 0;
    const auto attach = [&](GLenum attachment,
                            const FramebufferAttachment& att) {
        const GlTexture& texture = textures.at(att.texture.id);
        if (texture.arrayLayers > 1) {
            glNamedFramebufferTextureLayer(framebuffer, attachment,
                                           texture.name,
                                           static_cast<GLint>(att.mipLevel),
                                           static_cast<GLint>(att.arrayLayer));
        } else {
            glNamedFramebufferTexture(framebuffer, attachment, texture.name,
                                      static_cast<GLint>(att.mipLevel));
        }
        width = std::max(1u, texture.width >> att.mipLevel);
        height = std::max(1u, texture.height >> att.mipLevel);
    };

    vector<GLenum> drawBuffers;
    for (u32 i = 0; i < desc.colorAttachments.size(); ++i) {
        attach(GL_COLOR_ATTACHMENT0 + i, desc.colorAttachments[i]);
        drawBuffers.push_back(GL_COLOR_ATTACHMENT0 + i);
    }
    if (desc.depthAttachment.texture.id != 0) {
        attach(GL_DEPTH_ATTACHMENT, desc.depthAttachment);
    }
    if (drawBuffers.empty()) {
        // Depth-only pass (shadow maps): no color output at all.
        glNamedFramebufferDrawBuffer(framebuffer, GL_NONE);
        glNamedFramebufferReadBuffer(framebuffer, GL_NONE);
    } else {
        glNamedFramebufferDrawBuffers(
            framebuffer, static_cast<GLsizei>(drawBuffers.size()),
            drawBuffers.data());
    }

    const GLenum status =
        glCheckNamedFramebufferStatus(framebuffer, GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("createFramebuffer: incomplete (status 0x{:x})", status);
        glDeleteFramebuffers(1, &framebuffer);
        return {};
    }

    const u32 id = nextId++;
    framebuffers.emplace(
        id, GlFramebuffer { .name = framebuffer, .width = width,
                            .height = height });
    return { id };
}

// --- createPipeline (DSA) -----------------------------------------------------

PipelineHandle GlDevice46::createPipeline(const PipelineDesc& desc) {
    const auto shaderIt = shaders.find(desc.shader.id);
    if (shaderIt == shaders.end()) {
        LOG_ERROR("createPipeline: invalid shader handle");
        return {};
    }

    GlPipeline pipeline = makePipelineState(desc, shaderIt->second); // U2-03

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
