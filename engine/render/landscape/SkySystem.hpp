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

    void create(rhi::Device& device, ShaderLibrary& shaders);
    void destroy(rhi::Device& device);
    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // Draw last in the opaque pass (background pixels only).
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup);

    // Pure function of timeOfDay — headless-testable. `cloudCoverage` [0,1]
    // grays and dims the whole palette: heavy cover kills the direct sun,
    // mutes the sky and softens everything toward an overcast gloom.
    SkyState evaluate(f32 cloudCoverage = 0.0f) const;

    f32 timeOfDay { 10.5f }; // hours, [0, 24)

private:
    void buildPipeline(rhi::Device& device, ShaderLibrary& shaders);

    rhi::PipelineHandle pipeline {};
    u64 shaderGeneration { 0 };
};

} // namespace render
