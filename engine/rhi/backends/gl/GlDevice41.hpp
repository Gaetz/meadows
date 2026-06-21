#pragma once

#include "engine/rhi/backends/gl/GlDeviceBase.hpp"

namespace rhi {

// GL 4.1 backend: uses legacy bind-first paths (glGenBuffers/glBindBuffer,
// glGenTextures/glBindTexture, glVertexAttribPointer). Used on macOS and
// older hardware that do not support DSA (GL 4.5+).
class GlDevice41 final : public GlDeviceBase {
public:
    explicit GlDevice41(uptr<platform::GlContext> context,
                        platform::Window& window,
                        bool baseInstance,
                        GlDeviceBase::PFNBaseInstance pfnBaseInstance);

    BufferHandle  createBuffer(const BufferDesc& desc,
                               const void* initialData) override;
    void          updateBuffer(BufferHandle handle, const void* data, u64 size,
                               u64 offset) override;
    TextureHandle createTexture(const TextureDesc& desc,
                                const void* pixels) override;
    PipelineHandle createPipeline(const PipelineDesc& desc) override;

    void implBindTexture(u32 binding, u32 glTexId) override;
    void implBindVboSlot(const GlPipeline& p, u32 slot, u32 glBufId) override;
    void implBindEbo(const GlPipeline& p, u32 glBufId) override;
};

} // namespace rhi
