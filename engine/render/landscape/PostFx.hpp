#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/GpuProbe.hpp" // optional sub-pass timing (P0)
#include "engine/render/landscape/FrameUniforms.hpp"
#include "engine/rhi/Rhi.hpp"
#include "engine/rhi/UniqueHandle.hpp"

namespace rhi {
class CommandBuffer;
class Device;
}

namespace render {

class ShaderLibrary;

// Post-process chains that run between the water pass and the tonemap
// composite:
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
    // receiver group (the volumetric march taps it per step). Pass a
    // GpuProbe (+ its device) to time each sub-pass;
    // nullptr = no instrumentation. (No screen-space AO:
    // grounding = terrain light map + contact shadows + baked vertex AO.)
    // `giApplyGroup` (nullable) = the RC merged-cascade sampler: the
    // volumetric march's haze samples the GI field inside its volume.
    // `godRays` false skips that pass entirely (the tonemap multiplies
    // its texture by the zero intensity anyway).
    // `frameData` = this frame's RESOLVED uniforms — the froxel temporal
    // pass reads camera/view-proj/fog reach from it and keeps last
    // frame's copy for reprojection.
    void render(rhi::Device& device, rhi::CommandBuffer& cmd,
                const FrameUniforms& frameData,
                rhi::BindGroupHandle frameBindGroup,
                rhi::BindGroupHandle shadowBindGroup,
                rhi::BindGroupHandle giApplyGroup = {}, bool godRays = true,
                GpuProbe* probe = nullptr);

    // Auto-exposure — log-luminance 64² → mips →
    // 1×1 average, then the adaptation micro-pass (1×1 ping-pong with
    // asymmetric inertia; parameters ride FrameUbo slots — see adapt.frag).
    // Call after render(), before the tonemap. Skipped entirely while the
    // scene's toggle is off (the tonemap then multiplies by 1).
    void renderAutoExposure(rhi::Device& device, rhi::CommandBuffer& cmd,
                            rhi::BindGroupHandle frameBindGroup);

    // Screen-space contact shadows (Bend march over the depth
    // copy). The TOGGLE is the texture: when the scene turns
    // the feature off it calls clearContactShadows (neutral white) instead.
    // `shadowBindGroup` (the CSM receiver) lets the pass combine contact
    // and sun shadows as a MAX instead of multiplying.
    void renderContactShadows(rhi::CommandBuffer& cmd,
                              rhi::BindGroupHandle frameBindGroup,
                              rhi::BindGroupHandle shadowBindGroup);
    void clearContactShadows(rhi::CommandBuffer& cmd);

    // For the tonemap bind group. Contact hands out its BLURRED target
    // (its IGN jitter needs the filter).
    rhi::TextureHandle bloomTexture() const { return bloomTex[0]; }
    rhi::TextureHandle godRayTexture() const { return godRayTex; }
    rhi::TextureHandle volumetricTexture() const { return volumetricTex; }
    rhi::TextureHandle contactTexture() const { return contactBlurTex; }
    // The ping-pong side renderAutoExposure wrote LAST (the tonemap reads
    // it); the scene keeps one blit group per side.
    u32 exposureSide() const { return adaptSide; }
    rhi::TextureHandle exposureTexture(u32 side) const {
        return adaptTex[side];
    }

    bool ready() const { return bloomTex[0].id() != 0; }
    // The froxel path A/B (falls back to the 2D march when off or when
    // compute/volume caps are missing).
    bool froxelFog { true };
    // Temporal accumulation: fraction of the CURRENT sample per frame
    // (0.1 = ~90% history, converges in ~0.5 s; 1 = accumulation off).
    f32 froxelTemporalBlend { 0.1f };
    // Dust wisp amount: 0 = uniform dust, 1 = fully sparse drifting
    // sheets (value-noise density mask, interiors' uFogSunInfo.w only).
    f32 froxelDustNoise { 0.6f };
    bool froxelReady() const { return froxelInjectPipeline.id() != 0; }

private:
    void destroyTargets(rhi::Device& device);
    void buildPipelines(rhi::Device& device, ShaderLibrary& shaders);

    rhi::UniqueSampler linearSampler;
    // Contact-shadow march inputs: depth/alpha must NOT be filtered —
    // linear taps at thin silhouettes (grass blade tips) blend near and
    // far into phantom positions and half-exempt alpha, spraying false
    // contact shadow around the tips.
    rhi::UniqueSampler nearestSampler;

    array<rhi::UniqueTexture, kBloomLevels> bloomTex;
    array<rhi::UniqueFramebuffer, kBloomLevels> bloomFb;
    // Sampling bind groups: prefilter reads the scene, down[i] reads level
    // i-1, up[i] reads level i+1.
    rhi::UniqueBindGroup prefilterGroup;
    array<rhi::UniqueBindGroup, kBloomLevels> downGroup;
    array<rhi::UniqueBindGroup, kBloomLevels> upGroup;

    rhi::UniqueTexture godRayTex;
    rhi::UniqueFramebuffer godRayFb;
    rhi::UniqueBindGroup godRayGroup;

    rhi::UniqueTexture volumetricTex;
    rhi::UniqueFramebuffer volumetricFb;
    rhi::UniqueBindGroup volumetricGroup;

    // Froxel fog (docs/RENDERING.md V4/H4): fixed-size frustum volumes,
    // inject + integrate in compute, resolved into volumetricTex by a
    // fullscreen fetch — the tonemap composite is untouched. The 2D march
    // stays as the fallback (no compute caps) and the A/B.
    static constexpr u32 kFroxelX = 128, kFroxelY = 72, kFroxelZ = 64;
    // Scatter is a ping-pong pair: inject writes one side while sampling
    // the other as last frame's history (temporal accumulation).
    rhi::UniqueTexture froxelScatter[2];
    rhi::UniqueTexture froxelIntegrated;
    rhi::UniqueBindGroup froxelInjectGroup[2]; // images 12+13, history 7,
                                               // temporal ubo 9
    rhi::UniqueBuffer froxelTemporalUbo;
    u32 froxelSide { 0 };
    u32 froxelFrame { 0 };
    bool froxelHistoryValid { false };
    Mat4 froxelPrevViewProj { 1.0f };
    Vec3 froxelPrevCamera { 0.0f };
    f32 froxelPrevReach { 0.0f };
    rhi::UniqueBindGroup froxelApplyGroup;    // integrated sampler at 4
    rhi::UniquePipeline froxelInjectPipeline;
    rhi::UniquePipeline froxelIntegratePipeline;
    rhi::UniquePipeline froxelApplyPipeline;
    rhi::UniqueSampler froxelSampler;

    // Contact shadows + the 3x3 blur that filters their IGN
    // jitter — the tonemap taps the blurred target (contactTexture()).
    rhi::UniqueTexture contactTex;
    rhi::UniqueFramebuffer contactFb;
    rhi::UniqueBindGroup contactGroup;
    rhi::UniqueTexture contactBlurTex;
    rhi::UniqueFramebuffer contactBlurFb;
    rhi::UniqueBindGroup contactBlurGroup; // samples contactTex

    // Auto-exposure targets (window-size independent).
    rhi::UniqueTexture luminanceTex;
    rhi::UniqueFramebuffer luminanceFb;
    rhi::UniqueBindGroup luminanceGroup;
    array<rhi::UniqueTexture, 2> adaptTex;
    array<rhi::UniqueFramebuffer, 2> adaptFb;
    array<rhi::UniqueBindGroup, 2> adaptGroup;
    u32 adaptSide { 0 };

    rhi::UniquePipeline prefilterPipeline;
    rhi::UniquePipeline downPipeline;
    rhi::UniquePipeline upPipeline; // additive blend
    rhi::UniquePipeline godRayPipeline;
    rhi::UniquePipeline volumetricPipeline;
    rhi::UniquePipeline contactPipeline;
    rhi::UniquePipeline blurPipeline; // contact jitter filter (postblur)
    rhi::UniquePipeline luminancePipeline;
    rhi::UniquePipeline adaptPipeline;
    u64 shaderGeneration { 0 };
};

} // namespace render
