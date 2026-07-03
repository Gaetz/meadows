#pragma once

#include <unordered_map>

#include "engine/platform/GlContext.hpp"
#include "engine/rhi/Device.hpp"

namespace rhi {

class GlDeviceBase;

// Single command buffer for all GL backends. Delegates the three operations
// that differ between DSA (GL 4.6) and legacy (GL 4.1) to virtual impl*
// methods on the device, keeping this class branch-free.
class GlCommandBuffer final : public CommandBuffer {
public:
    explicit GlCommandBuffer(GlDeviceBase& device) : device { device } {}

    void beginRenderPass(const RenderPassDesc& desc) override;
    void endRenderPass() override;

    void setViewport(u32 x, u32 y, u32 width, u32 height) override;

    void setPipeline(PipelineHandle pipeline) override;
    void setBindGroup(u32 index, BindGroupHandle group) override;
    void setVertexBuffer(u32 slot, BufferHandle buffer) override;
    void setIndexBuffer(BufferHandle buffer, IndexFormat format) override;

    void draw(u32 vertexCount, u32 instanceCount, u32 firstVertex) override;
    void drawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex,
                     u32 firstInstance) override;

private:
    GlDeviceBase& device;
    u32 currentPipelineId { 0 };
    u32 indexByteSize { 4 };
    u32 glIndexType { 0 };
};

// Shared base for the GL backends. Owns all resource maps and provides the
// shared implementations. The two concrete subclasses (GlDevice46 for DSA,
// GlDevice41 for legacy bind-first) override only the 7 divergent methods.
class GlDeviceBase : public Device {
public:
    ~GlDeviceBase() override;

    CommandBuffer& beginFrame() override;
    void endFrame() override;
    Backend backend() const override { return Backend::OpenGL; }

    // --- Shared implementations (no branching) --------------------------------

    void destroyBuffer(BufferHandle handle) override;
    void destroyTexture(TextureHandle handle) override;
    ShaderHandle createShader(const ShaderDesc& desc) override;
    void destroyShader(ShaderHandle handle) override;
    void destroyPipeline(PipelineHandle handle) override;
    BindGroupHandle createBindGroup(const BindGroupDesc& desc) override;
    void destroyBindGroup(BindGroupHandle handle) override;

    // --- Pure virtual: divergent GL operations --------------------------------
    // Implemented by GlDevice46 (DSA) and GlDevice41 (legacy).

    BufferHandle  createBuffer(const BufferDesc& desc,
                               const void* initialData) override = 0;
    void          updateBuffer(BufferHandle handle, const void* data, u64 size,
                               u64 offset) override = 0;
    TextureHandle createTexture(const TextureDesc& desc,
                                const void* pixels) override = 0;
    PipelineHandle createPipeline(const PipelineDesc& desc) override = 0;

protected:
    friend class GlCommandBuffer;

    GlDeviceBase(uptr<platform::GlContext> context, platform::Window& window,
                 bool baseInstance,
                 platform::GlContext::ProcAddress pfnBaseInstance);

    // Stored as the generic GlContext::ProcAddress (void(*)()) to avoid pulling
    // GL types into the header; cast to the concrete sig at call sites in .cpp.
    using PFNBaseInstance = platform::GlContext::ProcAddress;

    // GlPipeline must be declared before implBind* so the parameter types
    // resolve correctly in both the virtual declarations and the overrides.
    struct GlPipeline {
        u32 program { 0 };
        u32 vao { 0 };
        BlendMode blend { BlendMode::Opaque };
        u32 glTopology { 0 };
        DepthState depth {};
        CullMode cull { CullMode::None };
        f32 depthBias { 0.0f };
        f32 depthBiasSlope { 0.0f };
        vector<u32> strides; // per vertex-buffer slot

        // GL 4.1 compatibility: attribute format info used by
        // GlDevice41::implBindVboSlot to re-issue glVertexAttribPointer.
        // GlDevice46 never populates these (they stay empty).
        struct AttribInfo41 {
            u32 location   { 0 };
            i32 components { 0 };
            u32 offset     { 0 };
            u32 slot       { 0 };
        };
        vector<AttribInfo41> attribs41;
        vector<u32>          divisors; // per slot
    };

public:
    // Called by GlCommandBuffer — avoid virtual dispatch per resource entry by
    // passing resolved GL names directly.
    virtual void implBindTexture(u32 binding, u32 glTexId) = 0;
    virtual void implBindVboSlot(const GlPipeline& p, u32 slot,
                                 u32 glBufId) = 0;
    virtual void implBindEbo(const GlPipeline& p, u32 glBufId) = 0;

protected:
    uptr<platform::GlContext> context;
    platform::Window& window;
    GlCommandBuffer commandBuffer;

    bool           baseInstance_ { false };
    PFNBaseInstance pfnDrawElementsInstancedBaseInstance_ { nullptr };

    // GL object registries, keyed by handle id (0 = invalid, shared counter).
    std::unordered_map<u32, u32>       buffers;    // id -> GL buffer
    std::unordered_map<u32, u32>       textures;   // id -> GL texture
    std::unordered_map<u32, u32>       shaders;    // id -> GL program
    std::unordered_map<u32, GlPipeline> pipelines;
    std::unordered_map<u32, BindGroupDesc> bindGroups;
    u32 nextId { 1 };
};

} // namespace rhi
