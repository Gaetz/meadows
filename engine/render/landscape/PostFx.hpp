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

    // For the tonemap bind group.
    rhi::TextureHandle bloomTexture() const { return bloomTex[0]; }
    rhi::TextureHandle godRayTexture() const { return godRayTex; }
    rhi::TextureHandle volumetricTexture() const { return volumetricTex; }
    rhi::TextureHandle ssaoTexture() const { return ssaoTex; }

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

    rhi::PipelineHandle prefilterPipeline {};
    rhi::PipelineHandle downPipeline {};
    rhi::PipelineHandle upPipeline {}; // additive blend
    rhi::PipelineHandle godRayPipeline {};
    rhi::PipelineHandle volumetricPipeline {};
    rhi::PipelineHandle ssaoPipeline {};
    u64 shaderGeneration { 0 };
};

} // namespace render
