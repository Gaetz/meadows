#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/Camera3D.hpp"
#include "engine/rhi/Rhi.hpp"

namespace rhi {
class Device;
}

namespace render {

// Cascaded shadow maps for the sun (brick 17): 3 cascades in one Depth32F
// 2048² texture array, each cascade a depth-only framebuffer over one layer.
// Cascades are fitted per frame around bounding spheres of camera-frustum
// slices (sphere fit = stable size under rotation) and snapped to the texel
// grid (no shimmer while translating). Receivers sample through a
// comparison sampler (hardware PCF) — see shaders/shadow.glsl.
class ShadowMapper {
public:
    static constexpr u32 kCascadeCount = 3;
    static constexpr u32 kDefaultResolution = 2048;

    struct Cascades {
        array<Mat4, kCascadeCount> viewProj {};
        array<f32, kCascadeCount> splitFar {};   // view distance covered
        array<f32, kCascadeCount> texelWorld {}; // world size of one texel
    };

    // `resolution` = texels per cascade side (dev sharpness knob — 4096
    // doubles definition everywhere for ~150 MB more; recreate to change).
    void create(rhi::Device& device,
                u32 resolution = kDefaultResolution);
    void destroy(rhi::Device& device);

    u32 resolution() const { return resolution_; }

    // Fits the cascades for this camera/sun. Pure math (no GPU).
    Cascades computeCascades(const Camera3D& camera, f32 aspect,
                             const Vec3& sunDirection) const;

    // Uploads each cascade's light matrix into its per-cascade UBO.
    void updateCascadeUbos(rhi::Device& device, const Cascades& cascades);

    // Depth-only target for cascade `i` (bind, then record casters).
    rhi::FramebufferHandle framebuffer(u32 i) const { return framebuffers_[i]; }
    // Caster bind group for cascade `i`: ShadowUbo (light matrix) binding 1.
    rhi::BindGroupHandle casterBindGroup(u32 i) const {
        return casterGroups[i];
    }
    // Receiver bind group: shadow map + comparison sampler on texture unit 1.
    rhi::BindGroupHandle receiverBindGroup() const { return receiverGroup; }

private:
    u32 resolution_ { kDefaultResolution };
    rhi::TextureHandle shadowMap {};
    rhi::SamplerHandle compareSampler {};
    array<rhi::FramebufferHandle, kCascadeCount> framebuffers_ {};
    array<rhi::BufferHandle, kCascadeCount> cascadeUbos {};
    array<rhi::BindGroupHandle, kCascadeCount> casterGroups {};
    rhi::BindGroupHandle receiverGroup {};
};

} // namespace render
