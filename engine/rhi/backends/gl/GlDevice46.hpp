#pragma once

#include "engine/rhi/backends/gl/GlDeviceBase.hpp"

namespace rhi {

// GL 4.6 backend: uses Direct State Access (DSA, core since GL 4.5) for all
// resource creation and command-buffer operations.
class GlDevice46 final : public GlDeviceBase {
public:
    explicit GlDevice46(uptr<platform::GlContext> context,
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
