#pragma once

#include <unordered_map>

#include "engine/platform/GlContext.hpp"
#include "engine/rhi/Device.hpp"

namespace rhi {

class GlDevice;

class GlCommandBuffer final : public CommandBuffer {
public:
    explicit GlCommandBuffer(GlDevice& device) : device { device } {}

    void beginRenderPass(const RenderPassDesc& desc) override;
    void endRenderPass() override;

    void setPipeline(PipelineHandle pipeline) override;
    void setBindGroup(u32 index, BindGroupHandle group) override;
    void setVertexBuffer(u32 slot, BufferHandle buffer) override;
    void setIndexBuffer(BufferHandle buffer, IndexFormat format) override;

    void draw(u32 vertexCount, u32 instanceCount, u32 firstVertex) override;
    void drawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex,
                     u32 firstInstance) override;

private:
    GlDevice& device;
    u32 currentPipelineId { 0 };
    u32 indexByteSize { 4 };
    u32 glIndexType { 0 };
};

// OpenGL 4.6 core backend (DSA, bindless). GL has no real command buffers or
// swapchain objects: "recording" executes immediately and present is a buffer
// swap. The Device interface hides both so callers stay backend-agnostic.
class GlDevice final : public Device {
public:
    static uptr<GlDevice> create(platform::Window& window);
    ~GlDevice() override;

    CommandBuffer& beginFrame() override;
    void endFrame() override;
    Backend backend() const override { return Backend::OpenGL; }

    BufferHandle createBuffer(const BufferDesc& desc,
                              const void* initialData) override;
    void updateBuffer(BufferHandle handle, const void* data, u64 size,
                      u64 offset) override;
    void destroyBuffer(BufferHandle handle) override;

    TextureHandle createTexture(const TextureDesc& desc,
                                const void* pixels) override;
    void destroyTexture(TextureHandle handle) override;

    ShaderHandle createShader(const ShaderDesc& desc) override;
    void destroyShader(ShaderHandle handle) override;

    PipelineHandle createPipeline(const PipelineDesc& desc) override;
    void destroyPipeline(PipelineHandle handle) override;

    BindGroupHandle createBindGroup(const BindGroupDesc& desc) override;
    void destroyBindGroup(BindGroupHandle handle) override;

private:
    friend class GlCommandBuffer;

    GlDevice(uptr<platform::GlContext> context, platform::Window& window);

    struct GlPipeline {
        u32 program { 0 };
        u32 vao { 0 };
        BlendMode blend { BlendMode::Opaque };
        u32 glTopology { 0 };
        vector<u32> strides; // per vertex-buffer slot
    };

    uptr<platform::GlContext> context;
    platform::Window& window;
    GlCommandBuffer commandBuffer;

    // GL object registries, keyed by handle id (0 = invalid, shared counter).
    std::unordered_map<u32, u32> buffers;   // id -> GL buffer
    std::unordered_map<u32, u32> textures;  // id -> GL texture
    std::unordered_map<u32, u32> shaders;   // id -> GL program
    std::unordered_map<u32, GlPipeline> pipelines;
    std::unordered_map<u32, BindGroupDesc> bindGroups;
    u32 nextId { 1 };
};

} // namespace rhi
