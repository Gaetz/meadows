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
    void setScissor(u32 x, u32 y, u32 width, u32 height) override;
    void clearScissor() override;
    void setFrontFace(FrontFace frontFace) override;

    void setPipeline(PipelineHandle pipeline) override;
    void setBindGroup(u32 index, BindGroupHandle group) override;
    void setPushConstants(const void* data, u32 size, u32 offset) override;
    void setVertexBuffer(u32 slot, BufferHandle buffer, u64 offset) override;
    void setIndexBuffer(BufferHandle buffer, IndexFormat format) override;

    void draw(u32 vertexCount, u32 instanceCount, u32 firstVertex) override;
    void drawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex,
                     u32 firstInstance) override;

    void copyTexture(TextureHandle src, TextureHandle dst) override;

    void copyBuffer(BufferHandle src, BufferHandle dst, u64 size,
                    u64 srcOffset, u64 dstOffset) override;
    void dispatch(u32 groupsX, u32 groupsY, u32 groupsZ) override;
    void memoryBarrier() override;

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
    const DeviceCaps& caps() const override { return caps_; }

    // --- Shared implementations (no branching) --------------------------------

    void destroyBuffer(BufferHandle handle) override;
    void destroyTexture(TextureHandle handle) override;
    ShaderHandle createShader(const ShaderDesc& desc) override;
    void destroyShader(ShaderHandle handle) override;
    void destroyPipeline(PipelineHandle handle) override;
    BindGroupHandle createBindGroup(const BindGroupDesc& desc) override;
    void destroyBindGroup(BindGroupHandle handle) override;
    void destroySampler(SamplerHandle handle) override;
    void destroyFramebuffer(FramebufferHandle handle) override;
    u64 nativeTextureId(TextureHandle handle) const override;

    // --- 3D-path features: logged stubs here, real implementations in
    // GlDevice46 (the future 4.1 degraded mode promotes them one by one).

    void generateMipmaps(TextureHandle handle) override;
    SamplerHandle createSampler(const SamplerDesc& desc) override;
    FramebufferHandle createFramebuffer(const FramebufferDesc& desc) override;

    // Compute: shared implementations, gated by caps_.computeShaders (the
    // 4.1 backend leaves the flag off and these log + return 0 / no-op).
    PipelineHandle createComputePipeline(
        const ComputePipelineDesc& desc) override;
    void readBuffer(BufferHandle handle, void* dst, u64 size,
                    u64 offset) override;

    // Fences (P1): GL sync objects, available since 3.2 — shared.
    FenceHandle insertFence() override;
    bool fenceReady(FenceHandle handle) override;
    void destroyFence(FenceHandle handle) override;

    // Timer queries (GPU-PERF P0): GL_TIMESTAMP, since 3.3 — shared.
    TimestampHandle insertTimestamp() override;
    bool timestampReady(TimestampHandle handle, u64& nanos) override;
    void destroyTimestamp(TimestampHandle handle) override;

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
        bool compute { false }; // program-only bind, no raster state
        BlendMode blend { BlendMode::Opaque };
        u32 glTopology { 0 };
        DepthState depth {};
        CullMode cull { CullMode::None };
        f32 depthBias { 0.0f };
        f32 depthBiasSlope { 0.0f };
        bool wireframe { false };
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

    // The raster-state half of createPipeline, identical in both backends
    // (audit U2-03): program + the 8 PipelineDesc fields. The subclasses
    // add only their VAO flavor (legacy re-pointer vs DSA).
    GlPipeline makePipelineState(const PipelineDesc& desc, u32 program) const;

public:
    // Called by GlCommandBuffer — avoid virtual dispatch per resource entry by
    // passing resolved GL names directly.
    virtual void implBindTexture(u32 binding, u32 glTexId) = 0;
    virtual void implBindVboSlot(const GlPipeline& p, u32 slot,
                                 u32 glBufId, u64 offset) = 0;
    virtual void implBindEbo(const GlPipeline& p, u32 glBufId) = 0;

protected:
    // Texture bookkeeping beyond the GL name: framebuffer creation and
    // (later) copyTexture need dimensions/layout.
    struct GlTexture {
        u32 name { 0 };
        u32 width { 0 };
        u32 height { 0 };
        u32 arrayLayers { 1 };
        u32 depth { 1 }; // > 1 = GL_TEXTURE_3D (G0): image binds layered
        TextureFormat format { TextureFormat::RGBA8 };
    };
    struct GlFramebuffer {
        u32 name { 0 };
        u32 width { 0 };  // attachment size at its selected mip:
        u32 height { 0 }; // beginRenderPass sets the viewport from these
    };

    uptr<platform::GlContext> context;
    platform::Window& window;
    GlCommandBuffer commandBuffer;

    bool           baseInstance_ { false };
    PFNBaseInstance pfnDrawElementsInstancedBaseInstance_ { nullptr };
    DeviceCaps caps_ {}; // set by the concrete backend's constructor

    // GL object registries, keyed by handle id (0 = invalid, shared counter).
    std::unordered_map<u32, u32>       buffers;    // id -> GL buffer
    // GL has no push constants: setPushConstants updates this buffer and
    // binds it at kPushConstantBinding. Created on first use (createBuffer is
    // the subclass's, so it cannot run in this constructor). 128 bytes = the
    // guaranteed Vulkan push-constant minimum, so a block that fits Vulkan
    // fits here too.
    BufferHandle pushConstants {};
    std::unordered_map<u32, GlTexture> textures;
    std::unordered_map<u32, u32>       shaders;    // id -> GL program
    std::unordered_map<u32, GlPipeline> pipelines;
    std::unordered_map<u32, BindGroupDesc> bindGroups;
    std::unordered_map<u32, u32>       samplers;   // id -> GL sampler
    std::unordered_map<u32, GlFramebuffer> framebuffers;
    std::unordered_map<u32, void*>     fences;     // id -> GLsync (P1)
    std::unordered_map<u32, u32> timerQueries; // id -> GL query (GPU-PERF)
    u32 nextId { 1 };
};

} // namespace rhi
