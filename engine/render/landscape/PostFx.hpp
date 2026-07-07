#pragma once

#include "engine/core/Defines.hpp"
#include "engine/rhi/Rhi.hpp"

namespace rhi {
class CommandBuffer;
class Device;
}

namespace render {

class ShaderLibrary;

// Post-process chains that run between the water pass and the tonemap
// composite (brick 21):
//  - Bloom: soft-threshold prefilter into a half-res HDR target, box
//    downsample down a 5-level pyramid, tent-upsample ADDITIVELY back up —
//    the level-0 result is sampled by the tonemap.
//  - God rays: screen-space light shafts — radial march toward the sun's
//    screen position over the sky-only scene radiance (uses the pre-water
//    scene snapshot), half-res.
// All targets track the window size (resize() from the scene).
class PostFx {
public:
    static constexpr u32 kBloomLevels = 5;

    void create(rhi::Device& device, ShaderLibrary& shaders);
    void destroy(rhi::Device& device);
    void refreshPipelines(rhi::Device& device, ShaderLibrary& shaders);

    // (Re)creates the pyramid and shaft targets for this window size, wiring
    // the scene's textures as inputs. Call whenever the offscreen targets
    // are (re)created.
    void resize(rhi::Device& device, u32 width, u32 height,
                rhi::TextureHandle sceneColor,
                rhi::TextureHandle sceneColorCopy,
                rhi::TextureHandle sceneDepthCopy);

    // Records the bloom chain, god-ray pass and volumetric shafts. Call
    // after the water pass, before the tonemap. `shadowBindGroup` is the CSM
    // receiver group (the volumetric march taps it per step).
    void render(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup,
                rhi::BindGroupHandle shadowBindGroup);

    // Brick 29 (chantier 6 B4): auto-exposure — log-luminance 64² → mips →
    // 1×1 average, then the adaptation micro-pass (1×1 ping-pong with
    // asymmetric inertia; parameters ride FrameUbo slots — see adapt.frag).
    // Call after render(), before the tonemap. Skipped entirely while the
    // scene's toggle is off (the tonemap then multiplies by 1).
    void renderAutoExposure(rhi::Device& device, rhi::CommandBuffer& cmd,
                            rhi::BindGroupHandle frameBindGroup);

    // Brick 33a — screen-space contact shadows (Bend march over the depth
    // copy, SSAO pattern). The TOGGLE is the texture: when the scene turns
    // the feature off it calls clearContactShadows (neutral white) instead.
    void renderContactShadows(rhi::CommandBuffer& cmd,
                              rhi::BindGroupHandle frameBindGroup);
    void clearContactShadows(rhi::CommandBuffer& cmd);

    // For the tonemap bind group.
    rhi::TextureHandle bloomTexture() const { return bloomTex[0]; }
    rhi::TextureHandle godRayTexture() const { return godRayTex; }
    rhi::TextureHandle volumetricTexture() const { return volumetricTex; }
    rhi::TextureHandle ssaoTexture() const { return ssaoTex; }
    rhi::TextureHandle contactTexture() const { return contactTex; }
    // The ping-pong side renderAutoExposure wrote LAST (the tonemap reads
    // it); the scene keeps one blit group per side.
    u32 exposureSide() const { return adaptSide; }
    rhi::TextureHandle exposureTexture(u32 side) const {
        return adaptTex[side];
    }

    bool ready() const { return bloomTex[0].id != 0; }

private:
    void destroyTargets(rhi::Device& device);
    void buildPipelines(rhi::Device& device, ShaderLibrary& shaders);

    rhi::SamplerHandle linearSampler {};

    array<rhi::TextureHandle, kBloomLevels> bloomTex {};
    array<rhi::FramebufferHandle, kBloomLevels> bloomFb {};
    // Sampling bind groups: prefilter reads the scene, down[i] reads level
    // i-1, up[i] reads level i+1.
    rhi::BindGroupHandle prefilterGroup {};
    array<rhi::BindGroupHandle, kBloomLevels> downGroup {};
    array<rhi::BindGroupHandle, kBloomLevels> upGroup {};

    rhi::TextureHandle godRayTex {};
    rhi::FramebufferHandle godRayFb {};
    rhi::BindGroupHandle godRayGroup {};

    rhi::TextureHandle volumetricTex {};
    rhi::FramebufferHandle volumetricFb {};
    rhi::BindGroupHandle volumetricGroup {};

    rhi::TextureHandle ssaoTex {};
    rhi::FramebufferHandle ssaoFb {};
    rhi::BindGroupHandle ssaoGroup {};

    // Brick 33a: contact shadows.
    rhi::TextureHandle contactTex {};
    rhi::FramebufferHandle contactFb {};
    rhi::BindGroupHandle contactGroup {};

    // Brick 29: auto-exposure targets (window-size independent).
    rhi::TextureHandle luminanceTex {};
    rhi::FramebufferHandle luminanceFb {};
    rhi::BindGroupHandle luminanceGroup {};
    array<rhi::TextureHandle, 2> adaptTex {};
    array<rhi::FramebufferHandle, 2> adaptFb {};
    array<rhi::BindGroupHandle, 2> adaptGroup {};
    u32 adaptSide { 0 };

    rhi::PipelineHandle prefilterPipeline {};
    rhi::PipelineHandle downPipeline {};
    rhi::PipelineHandle upPipeline {}; // additive blend
    rhi::PipelineHandle godRayPipeline {};
    rhi::PipelineHandle volumetricPipeline {};
    rhi::PipelineHandle ssaoPipeline {};
    rhi::PipelineHandle contactPipeline {};
    rhi::PipelineHandle luminancePipeline {};
    rhi::PipelineHandle adaptPipeline {};
    u64 shaderGeneration { 0 };
};

} // namespace render
