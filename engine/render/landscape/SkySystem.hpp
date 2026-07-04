#pragma once

#include "engine/core/Defines.hpp"
#include "engine/rhi/Rhi.hpp"

#include <glm/glm.hpp>

namespace rhi {
class CommandBuffer;
class Device;
}

namespace render {

class ShaderLibrary;

// Analytic gradient sky + day/night cycle (brick 9). Evaluates the sun and
// sky palette from a time of day; the scene pushes the result into
// FrameUniforms so every shader (terrain lighting now, fog later) reads the
// SAME sun/sky the dome is painted with. The dome itself is one fullscreen
// triangle drawn after the opaque pass at far depth (LessEqual, no write):
// only background pixels get shaded.
class SkySystem {
public:
    // Sun/sky palette for one moment of the day, in LINEAR HDR: the sun
    // exceeds 1 so the filmic tonemap rolls highlights off, and the disc is
    // bright enough to bloom later.
    struct SkyState {
        Vec3 sunDirection {};
        Vec3 sunColor {};
        Vec3 glowColor {};  // sky halo/afterglow — outlives the sun disc
        Vec3 ambientColor {};
        Vec3 zenithColor {};
        Vec3 horizonColor {};     // sun side of the horizon ring
        Vec3 horizonFarColor {};  // opposite side (night arrives there first)
        f32 sunDiscIntensity { 12.0f };
    };

    // Baked cloud field: the drifting 2D density around the camera,
    // rendered once per frame so shadow consumers (terrain/tree/grass
    // lighting, volumetric march) sample a texture instead of a 4-octave
    // FBM per evaluation.
    static constexpr u32 kCloudMapSize = 512;
    static constexpr f32 kCloudMapSpan = 8192.0f; // meters covered

    void create(rhi::Device& device, ShaderLibrary& shaders);
    void destroy(rhi::Device& device);
    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // Renders the cloud field for this frame. Call before any pass that
    // lights with cloud shadows.
    void bakeCloudMap(rhi::CommandBuffer& cmd,
                      rhi::BindGroupHandle frameBindGroup);
    // Texture unit 2 for every consumer; bind once per render pass.
    rhi::BindGroupHandle cloudMapBindGroup() const { return cloudMapGroup; }

    // Draw last in the opaque pass (background pixels only).
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup);

    // Weather modifiers layered over the time-of-day palette (brick 24).
    // `cloudCoverage` [0,1] grays and dims everything toward overcast gloom;
    // the rest lets a weather state color-grade the moment: `warmth` reddens
    // the low-sun palette (haze makes sunsets burn — Porco Rosso skies),
    // `saturation` washes the world toward gray (overcast, storm), and the
    // intensity multipliers dim the light beyond what coverage does.
    struct Weather {
        f32 cloudCoverage { 0.0f };
        f32 sunIntensity { 1.0f };
        f32 ambientIntensity { 1.0f };
        f32 saturation { 1.0f };
        f32 warmth { 0.0f };
    };

    // Pure function of timeOfDay — headless-testable.
    SkyState evaluate(const Weather& weather = {}) const;

    f32 timeOfDay { 10.5f }; // hours, [0, 24)

private:
    void buildPipeline(rhi::Device& device, ShaderLibrary& shaders);

    rhi::PipelineHandle pipeline {};
    u64 shaderGeneration { 0 };

    rhi::TextureHandle cloudMap {};
    rhi::FramebufferHandle cloudMapFb {};
    rhi::SamplerHandle cloudMapSampler {};
    rhi::BindGroupHandle cloudMapGroup {};
    rhi::PipelineHandle bakePipeline {};
    u64 bakeShaderGeneration { 0 };
};

} // namespace render
