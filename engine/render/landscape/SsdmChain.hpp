#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/render/WatchedPipeline.hpp"
#include "engine/rhi/Rhi.hpp"
#include "engine/rhi/UniqueHandle.hpp"

namespace rhi {
class CommandBuffer;
class Device;
}

namespace render {

// Screen-space displacement mapping (ssdm_*.frag — Lobel 2008): the
// alpha-packed relief of the scene is scattered in screen space —
// flow (per-pixel displacement + displaced depth) -> bounds quadtree
// (kLevels halving levels) -> resolve (nearest-wins, gather fallback)
// back into the scene target. Crests extrude over their neighbors (sky
// included); holes fall back to the gather (pits keep digging).
// Reads the pre-resolve scene snapshot (color + depth copies) and
// writes the scene color target in place. Targets track the scene
// size and the mode (resize() from the scene target's owner).
class SsdmChain {
public:
    static constexpr u32 kLevels = 5;

    // The stored knob stays RenderTuning::ssdmMode (an i32 the panel's
    // combo writes); these name its values. Half = chain at half the
    // scene resolution + edge-aware upsample of the moved pixels only;
    // Full = chain at scene resolution, kept for its crispness.
    enum Mode : i32 { kOff = 0, kHalf = 1, kFull = 2 };

    // Shader loads only — the targets come with resize().
    void create(rhi::Device& device, ShaderLibrary& shaders);
    void destroy(rhi::Device& device);

    // (Re)creates the chain targets for this scene size at the mode's
    // resolution (full / half / a token 4x4 when off — the memory is
    // not free at Retina sizes), sampling `sceneColorCopy` /
    // `sceneDepthCopy` through `sampler`. Call whenever the scene
    // target is (re)created.
    void resize(rhi::Device& device, i32 mode, u32 width, u32 height,
                rhi::TextureHandle sceneColorCopy,
                rhi::TextureHandle sceneDepthCopy,
                rhi::SamplerHandle sampler);
    void destroyTargets(rhi::Device& device);
    bool ready() const { return flowFb.id() != 0; }

    // Records the chain into `sceneFb` (loaded, never cleared). The
    // caller refreshes the color copy bound at resize() first (the
    // pre-water one was consumed). Pipelines are ensured here, lazily:
    // first use + shader hot reload, nothing built while the mode is off.
    void record(rhi::Device& device, rhi::CommandBuffer& cmd,
                const ShaderLibrary& shaders, i32 mode,
                rhi::FramebufferHandle sceneFb,
                rhi::BindGroupHandle frameBindGroup);

private:
    void ensurePipelines(rhi::Device& device, const ShaderLibrary& shaders);

    WatchedPipeline flowPipeline;
    WatchedPipeline bounds0Pipeline;
    WatchedPipeline downPipeline;
    WatchedPipeline resolvePipeline;
    rhi::UniqueTexture flowTex;
    rhi::UniqueFramebuffer flowFb;
    rhi::UniqueBindGroup flowGroup;
    array<rhi::UniqueTexture, kLevels> boundsTex;
    array<rhi::UniqueFramebuffer, kLevels> boundsFb;
    array<rhi::UniqueBindGroup, kLevels> boundsGroup;
    rhi::UniqueBindGroup resolveGroup;
    // Half mode: the resolve lands in this chain-res intermediate
    // (alpha = moved flag), then the upsample rewrites the touched
    // full-res pixels only.
    WatchedPipeline resolveHalfPipeline;
    WatchedPipeline upsamplePipeline;
    rhi::UniqueTexture halfTex;
    rhi::UniqueFramebuffer halfFb;
    rhi::UniqueBindGroup upsampleGroup;
};

} // namespace render
