#pragma once

#include <unordered_map>

#include "engine/core/Defines.hpp"
#include "engine/render/Camera2D.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

struct Sprite {
    Vec2 position {};                        // world units, sprite center
    Vec2 size { 1.0f, 1.0f };                // world units
    f32 rotation { 0.0f };                   // radians, CCW
    Vec4 uvRect { 0.0f, 0.0f, 1.0f, 1.0f };  // u0, v0, u1, v1
    Vec4 tint { 1.0f, 1.0f, 1.0f, 1.0f };
    rhi::TextureHandle texture {};
};

// Instanced quad renderer. Sprites draw in submission order (painter's
// algorithm — no depth buffer in the 2D phase); consecutive sprites sharing a
// texture collapse into one instanced draw.
class SpriteRenderer {
public:
    // Returns nullptr (with a logged error) if GPU resource creation fails.
    static uptr<SpriteRenderer> create(rhi::Device& device);
    ~SpriteRenderer();

    void begin(const Camera2D& camera, f32 aspect);
    void draw(const Sprite& sprite);
    // Uploads the frame's camera UBO + instance data. Call AFTER the last
    // draw() and BEFORE the render pass opens: buffer updates inside a
    // pass fall back to an in-place write on Vulkan, which races the
    // frame still in flight (see docs/VULKAN.md).
    void upload();
    // Records the draw calls (upload() must have run this frame).
    void end(rhi::CommandBuffer& cmd);

    static constexpr u32 kMaxSprites = 8192;

private:
    explicit SpriteRenderer(rhi::Device& device);

    rhi::BindGroupHandle bindGroupFor(rhi::TextureHandle texture);

    struct Instance {
        Vec4 posSize;  // xy = world position, zw = world size
        Vec4 uvRect;
        Vec4 tint;
        f32 rotation;
    };

    struct Batch {
        u32 textureId;
        u32 firstInstance;
        u32 instanceCount;
    };

    rhi::Device& device;
    rhi::BufferHandle quadVertices {};
    rhi::BufferHandle quadIndices {};
    rhi::BufferHandle instanceBuffer {};
    rhi::BufferHandle cameraUbo {};
    rhi::ShaderHandle shader {};
    rhi::PipelineHandle pipeline {};
    rhi::TextureHandle whiteTexture {};  // fallback for untextured sprites

    // One bind group per texture seen so far (camera UBO + texture).
    std::unordered_map<u32, rhi::BindGroupHandle> bindGroups;

    vector<Instance> instances;
    vector<Batch> batches;
    Mat4 pendingViewProj { 1.0f }; // begin() stores, upload() writes
    bool overflowWarned { false };
};

} // namespace render
