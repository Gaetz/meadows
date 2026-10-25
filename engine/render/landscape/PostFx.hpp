#pragma once

#include <initializer_list>

#include "engine/core/Defines.hpp"
#include "engine/render/GpuProbe.hpp" // optional sub-pass timing (P0)
#include "engine/render/landscape/FrameUniforms.hpp"
#include "engine/rhi/Rhi.hpp"
#include "engine/rhi/UniqueHandle.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/render/WatchedPipeline.hpp"

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
    // Frees every GPU resource; the public tuning knobs survive (they are
    // the user's settings, not resources).
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
    // nullptr = no instrumentation.
    // `giApplyGroup` (nullable) = the RC merged-cascade sampler: the
    // volumetric march's haze samples the GI field inside its volume.
    // `godRays` false skips that pass entirely (the tonemap multiplies
    // its texture by the zero intensity anyway).
    // `frameData` = this frame's RESOLVED uniforms — the froxel temporal
    // pass reads camera/view-proj/fog reach from it and keeps last
    // frame's copy for reprojection.
    // `cloudMapGroup` = the baked cloud field, bound at its OWN slot for
    // the fog passes' cloudShadowFactor — relying on a group an earlier
    // pass bound at slot 3 loses it on Vulkan (an overwritten slot drops
    // its bindings; GL's sticky texture units masked this).
    void render(rhi::Device& device, rhi::CommandBuffer& cmd,
                const FrameUniforms& frameData,
                rhi::BindGroupHandle frameBindGroup,
                rhi::BindGroupHandle shadowBindGroup,
                rhi::BindGroupHandle giApplyGroup = {}, bool godRays = true,
                GpuProbe* probe = nullptr,
                rhi::BindGroupHandle cloudMapGroup = {});

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

    // Ground mist raymarch (mist.frag, docs/RENDERING.md §3.5) into its
    // own half-res target; the tonemap composites it BEFORE the fog term.
    // The toggle is the texture (clearMist = neutral), like the contact
    // shadows. `cloudMapGroup`/`mistMapGroup` are bound at their own
    // slots — the pass must not rely on groups earlier passes bound.
    void renderMist(rhi::Device& device, rhi::CommandBuffer& cmd,
                    const FrameUniforms& frameData,
                    rhi::BindGroupHandle frameBindGroup,
                    rhi::BindGroupHandle shadowBindGroup,
                    rhi::BindGroupHandle giApplyGroup,
                    rhi::BindGroupHandle cloudMapGroup,
                    rhi::BindGroupHandle mistMapGroup,
                    rhi::BindGroupHandle noiseVolumeGroup = {});
    void clearMist(rhi::CommandBuffer& cmd);
    // Temporal accumulation on the mist target: fraction of the CURRENT
    // sample per frame (0.15 = ~85% history; 1 = accumulation off).
    f32 mistTemporalBlend { 0.15f };

    // Volumetric sky clouds (skyclouds.frag, §8) — same family as the
    // mist: ½-res target + history blit + temporal EMA; the tonemap
    // composites it FIRST (binding 7, farthest medium). The toggle is
    // the texture (clearSkyClouds = neutral).
    void renderSkyClouds(rhi::Device& device, rhi::CommandBuffer& cmd,
                         const FrameUniforms& frameData,
                         rhi::BindGroupHandle frameBindGroup,
                         rhi::BindGroupHandle noiseVolumeGroup,
                         rhi::BindGroupHandle cloudMapGroup = {});
    void clearSkyClouds(rhi::CommandBuffer& cmd);
    f32 cloudTemporalBlend { 0.12f };

    // For the tonemap bind group. Contact hands out its BLURRED target
    // (its IGN jitter needs the filter).
    rhi::TextureHandle bloomTexture() const { return bloom[0].tex; }
    rhi::TextureHandle godRayTexture() const { return godRay.tex; }
    rhi::TextureHandle volumetricTexture() const { return volumetric.tex; }
    rhi::TextureHandle mistTexture() const { return mist.out.tex; }
    // The tonemap taps the display-BLURRED target; the temporal history
    // stays sharp (the froxel display-blur lesson).
    rhi::TextureHandle skyCloudsTexture() const {
        return skyCloudBlur.target.tex;
    }
    rhi::TextureHandle contactTexture() const {
        return contact.blur.target.tex;
    }
    // Half-res SSAO (ssao.frag — the contact pattern's sibling): the
    // tonemap taps the BLURRED target; clearSsao() = toggle off.
    void renderSsao(rhi::CommandBuffer& cmd,
                    rhi::BindGroupHandle frameBindGroup);
    void clearSsao(rhi::CommandBuffer& cmd);
    rhi::TextureHandle ssaoTexture() const { return ssao.blur.target.tex; }
    // The ping-pong side renderAutoExposure wrote LAST (the tonemap reads
    // it); the scene keeps one blit group per side.
    u32 exposureSide() const { return adaptSide; }
    rhi::TextureHandle exposureTexture(u32 side) const {
        return adapt[side].tex;
    }

    bool ready() const { return bloom[0].ready(); }
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
    // A color render target: the texture plus the framebuffer that draws
    // into it (sampled + render attachment, clamp-to-edge).
    struct RenderTarget {
        rhi::UniqueTexture tex;
        rhi::UniqueFramebuffer fb;

        void create(rhi::Device& device, u32 width, u32 height,
                    rhi::TextureFormat format,
                    rhi::FilterMode filter = rhi::FilterMode::Linear,
                    u32 mipLevels = 1);
        void reset();
        bool ready() const { return tex.id() != 0; }
    };

    // A target produced from ONE sampled source by a fullscreen pass — the
    // history copies and the display blurs. `source` samples the texture
    // the target derives from, bound at slot 0 of the copy/blur shaders.
    struct DerivedTarget {
        RenderTarget target;
        rhi::UniqueBindGroup source;

        void create(rhi::Device& device, u32 width, u32 height,
                    rhi::TextureFormat format, rhi::TextureHandle sourceTex,
                    rhi::SamplerHandle sampler);
        void reset();
        // The fullscreen draw into the target (no clear).
        void record(rhi::CommandBuffer& cmd,
                    rhi::PipelineHandle pipeline) const;
    };

    // Temporal EMA reprojection state shared by the media passes (mist,
    // sky clouds, froxels): last frame's view-proj/camera for the history
    // lookup, the std140 ubo the shaders read it from, and the validity
    // gate. `prevReach` is the froxels' reach-aware history slice; the
    // screen-space media leave it at zero.
    struct TemporalHistory {
        rhi::UniqueBuffer ubo;
        Mat4 prevViewProj { 1.0f };
        Vec3 prevCamera { 0.0f };
        f32 prevReach { 0.0f };
        u32 frame { 0 };
        bool valid { false };

        void create(rhi::Device& device);
        // Fraction of the CURRENT sample this frame: `blend` clamped, or
        // the full-weight 1 when the history can't be reprojected (first
        // frame, invalidated, or a camera jump).
        f32 alpha(f32 blend, const Vec3& camera) const;
        // Uploads this frame's reprojection uniforms (`cameraW` rides
        // prevCamera.w, `dustNoise` temporalInfo.z) and advances the
        // frame counter the shaders' jitter sequence reads.
        void upload(rhi::Device& device, f32 alpha, f32 cameraW,
                    f32 dustNoise);
        // Records this frame as next frame's history.
        void commit(const Mat4& viewProj, const Vec3& camera, f32 reach,
                    bool nowValid);
    };

    // A screen-space medium raymarched into its own half-res target with
    // a temporal EMA against last frame's result: the ground mist and the
    // volumetric sky clouds. History is a plain copy of last frame's
    // target (a blit after the pass) — no ping-pong, so the tonemap blit
    // group stays static. `group` = scene depth (0) + history (10) +
    // temporal ubo (3). The ubo outlives the window-size targets.
    struct MediaPass {
        RenderTarget out;
        TemporalHistory temporal;
        DerivedTarget history; // samples `out`
        rhi::UniqueBindGroup group;

        void createTargets(rhi::Device& device, u32 width, u32 height,
                           rhi::TextureHandle sceneDepthCopy,
                           rhi::SamplerHandle sampler);
        void resetTargets();
    };

    // A half-res pass whose IGN jitter needs the 3x3 blur before the
    // tonemap taps it: contact shadows and SSAO. `group` = the march's
    // inputs; the blur samples `raw`.
    struct FilteredPass {
        RenderTarget raw;
        rhi::UniqueBindGroup group;
        DerivedTarget blur; // samples `raw`

        void reset();
    };

    struct SlotGroup {
        u32 slot;
        rhi::BindGroupHandle group;
    };

    // The shared media recording: temporal uniforms, the raymarch into
    // `pass.out` (frame group at 0, the pass group at 1, then `groups` in
    // order — zero handles skipped), the history blit, the commit.
    void renderMedia(rhi::Device& device, rhi::CommandBuffer& cmd,
                     const FrameUniforms& frameData, MediaPass& pass,
                     rhi::PipelineHandle pipeline, f32 blend,
                     rhi::BindGroupHandle frameBindGroup,
                     std::initializer_list<SlotGroup> groups);
    void destroyTargets(rhi::Device& device);

    rhi::UniqueSampler linearSampler;
    // Contact-shadow march inputs: depth/alpha must NOT be filtered —
    // linear taps at thin silhouettes (grass blade tips) blend near and
    // far into phantom positions and half-exempt alpha, spraying false
    // contact shadow around the tips.
    rhi::UniqueSampler nearestSampler;

    array<RenderTarget, kBloomLevels> bloom;
    // Sampling bind groups: prefilter reads the scene, down[i] reads level
    // i-1, up[i] reads level i+1.
    rhi::UniqueBindGroup prefilterGroup;
    array<rhi::UniqueBindGroup, kBloomLevels> downGroup;
    array<rhi::UniqueBindGroup, kBloomLevels> upGroup;

    RenderTarget godRay;
    rhi::UniqueBindGroup godRayGroup;

    RenderTarget volumetric;
    rhi::UniqueBindGroup volumetricGroup;

    MediaPass mist;

    // Volumetric sky clouds — the mist family plus a display-only 3x3
    // blur (postblur) the tonemap taps; the EMA history stays sharp.
    MediaPass skyCloud;
    DerivedTarget skyCloudBlur; // samples skyCloud.out

    // Froxel fog (docs/RENDERING.md V4/H4): fixed-size frustum volumes,
    // inject + integrate in compute, resolved into the volumetric target
    // by a fullscreen fetch — the tonemap composite is untouched. The 2D
    // march stays as the fallback (no compute caps) and the A/B.
    static constexpr u32 kFroxelX = 128, kFroxelY = 72, kFroxelZ = 64;
    rhi::UniqueSampler froxelSampler;
    // Scatter is a ping-pong pair: inject writes one side while sampling
    // the other as last frame's history (temporal accumulation).
    rhi::UniqueTexture froxelScatter[2];
    rhi::UniqueTexture froxelIntegrated;
    TemporalHistory froxelTemporal;
    rhi::UniqueBindGroup froxelInjectGroup[2]; // images 12+13, history 7,
                                               // temporal ubo 9
    u32 froxelSide { 0 };
    rhi::UniqueBindGroup froxelApplyGroup;    // integrated sampler at 4
    WatchedPipeline froxelInjectPipeline;
    WatchedPipeline froxelIntegratePipeline;
    WatchedPipeline froxelApplyPipeline;

    // Contact shadows + the 3x3 blur that filters their IGN
    // jitter — the tonemap taps the blurred target (contactTexture()).
    FilteredPass contact;
    FilteredPass ssao;
    WatchedPipeline ssaoPipeline;

    // Auto-exposure targets (window-size independent).
    RenderTarget luminance;
    rhi::UniqueBindGroup luminanceGroup;
    array<RenderTarget, 2> adapt;
    array<rhi::UniqueBindGroup, 2> adaptGroup;
    u32 adaptSide { 0 };

    WatchedPipeline prefilterPipeline;
    WatchedPipeline downPipeline;
    WatchedPipeline upPipeline; // additive blend
    WatchedPipeline godRayPipeline;
    WatchedPipeline volumetricPipeline;
    WatchedPipeline contactPipeline;
    WatchedPipeline mistPipeline;
    WatchedPipeline skyCloudPipeline;
    WatchedPipeline copyPipeline; // history blit (postcopy)
    WatchedPipeline blurPipeline; // contact jitter filter (postblur)
    WatchedPipeline luminancePipeline;
    WatchedPipeline adaptPipeline;
};

} // namespace render
