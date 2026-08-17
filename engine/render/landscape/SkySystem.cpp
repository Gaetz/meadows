#include "engine/render/landscape/SkySystem.hpp"

#include <cmath>

#include <glm/gtc/constants.hpp>

#include "engine/render/ShaderLibrary.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {

constexpr const char* kSkyShader = "sky";
constexpr const char* kCloudBakeShader = "cloud_bake";

// The palette below is authored in display (sRGB-ish) space — the intuitive
// space to pick colors in. The HDR pipeline lights in linear and re-encodes
// at the tonemap pass, so authored colors convert on the way in.
Vec3 linearize(const Vec3& srgb) {
    return glm::pow(srgb, Vec3 { 2.2f });
}

// Sun radiance in daylight: > 1 on purpose, so lit slopes exceed the display
// range and the filmic curve rolls them off instead of clipping.
constexpr f32 kSunIntensity = 2.6f;

} // namespace

SkySystem::SkyState SkySystem::evaluate() const { return evaluate(Weather {}); }

SkySystem::SkyState SkySystem::evaluate(const Weather& weather) const {
    // Sun path: rises +X at 6h, zenith at 12h, sets -X at 18h; a slight +Z
    // tilt keeps noon shadows from degenerating to a point.
    const f32 theta =
        (timeOfDay - 6.0f) / 12.0f * glm::pi<f32>();
    const Vec3 sunDirection = glm::normalize(
        Vec3 { std::cos(theta), std::sin(theta), 0.22f });
    const f32 elevation = sunDirection.y;

    // day: overall daylight amount (ambient + base sky palette).
    const f32 day = glm::smoothstep(-0.08f, 0.25f, elevation);
    // sunUp: the sun stays BRIGHT until the disc actually dips — a cinematic
    // sunset is a red sun, not a dim one.
    const f32 sunUp = glm::smoothstep(-0.06f, 0.03f, elevation);
    // golden: low sun (golden hour); reddening: the last degrees above the
    // horizon, where the disc goes deep red.
    const f32 golden = 1.0f - glm::smoothstep(0.08f, 0.45f, elevation);
    const f32 reddening = 1.0f - glm::smoothstep(-0.02f, 0.15f, elevation);
    // twilight: peaks while the sun crosses the horizon and decays slowly
    // below it — l'heure entre chien et loup: the disc is gone but its
    // scattered light is not (fades out ~50 min after sunset).
    const f32 twilight = glm::smoothstep(-0.22f, -0.02f, elevation) *
                         (1.0f - glm::smoothstep(0.10f, 0.35f, elevation));

    // Dawn/dusk asymmetry. Overnight the air cools and settles: fewer
    // aerosols, more humidity — mornings are clearer and PASTEL (rose,
    // salmon, pale lavender). Daytime convection loads the evening air with
    // dust — sunsets are the fiery orange/indigo ones. `morning` fades over
    // midday; both seams (noon, midnight) sit where twilight is ~0.
    const f32 morning = 1.0f - glm::smoothstep(10.0f, 14.0f, timeOfDay);
    const Vec3 goldenColor = glm::mix(Vec3 { 1.00f, 0.55f, 0.20f },
                                      Vec3 { 1.00f, 0.72f, 0.46f }, morning);
    const Vec3 redColor = glm::mix(Vec3 { 1.00f, 0.22f, 0.05f },
                                   Vec3 { 1.00f, 0.46f, 0.32f }, morning);
    const Vec3 duskZenith = glm::mix(Vec3 { 0.16f, 0.09f, 0.46f },
                                     Vec3 { 0.32f, 0.30f, 0.60f }, morning);
    const Vec3 duskHorizon = glm::mix(Vec3 { 1.00f, 0.38f, 0.10f },
                                      Vec3 { 1.00f, 0.62f, 0.54f }, morning);
    const Vec3 duskAmbient = glm::mix(Vec3 { 0.28f, 0.20f, 0.30f },
                                      Vec3 { 0.30f, 0.27f, 0.35f }, morning);

    SkyState state;
    state.sunDirection = sunDirection;
    Vec3 sun = glm::mix(Vec3 { 1.00f, 0.97f, 0.90f }, goldenColor, golden);
    sun = glm::mix(sun, redColor, reddening);
    state.sunColor = linearize(sun) * (sunUp * kSunIntensity);
    // The halo/afterglow follows the same palette but outlives the disc;
    // cleaner morning air scatters less, so the dawn glow is softer.
    state.glowColor = linearize(sun) * glm::max(sunUp, twilight) *
                      (1.0f - 0.25f * morning) * 1.4f;

    Vec3 zenith = glm::mix(Vec3 { 0.015f, 0.025f, 0.060f },
                           Vec3 { 0.22f, 0.45f, 0.80f }, day);
    // Indigo (dusk) / pale lavender (dawn) band overhead, blending into the
    // warm horizon as a purple-to-rose gradient midway down.
    zenith = glm::mix(zenith, duskZenith, twilight * 0.70f);
    state.zenithColor = linearize(zenith);

    const Vec3 horizonBase = glm::mix(Vec3 { 0.040f, 0.050f, 0.100f },
                                      Vec3 { 0.60f, 0.75f, 0.90f }, day);
    // Sun side gets the warm twilight band; the opposite horizon is already
    // sliding into night while the sun sets.
    state.horizonColor =
        linearize(glm::mix(horizonBase, duskHorizon, twilight * 0.90f));
    state.horizonFarColor =
        linearize(glm::mix(horizonBase, Vec3 { 0.040f, 0.050f, 0.100f },
                           twilight * 0.45f));

    Vec3 ambient = glm::mix(Vec3 { 0.045f, 0.055f, 0.095f },
                            Vec3 { 0.34f, 0.39f, 0.47f }, day);
    // Between dog and wolf the world stays readable: a dim indigo-warm
    // ambient lingers with the afterglow instead of dropping to night.
    ambient = glm::mix(ambient, duskAmbient, twilight * 0.45f);
    state.ambientColor = linearize(ambient);

    // Overcast: heavy cover eats the direct sun first (real overcast light is
    // all-diffuse), grays the sky toward its own luminance, and dims the
    // ambient a little — the world turns gloomy, not just spotted.
    const f32 overcast = glm::clamp(weather.cloudCoverage, 0.0f, 1.0f);
    if (overcast > 0.0f) {
        const auto grayed = [&](const Vec3& c) {
            const f32 luma = glm::dot(c, Vec3 { 0.299f, 0.587f, 0.114f });
            return glm::mix(c, Vec3 { luma * 0.85f + 0.015f },
                            overcast * 0.65f);
        };
        state.sunColor *= 1.0f - 0.75f * overcast;
        state.glowColor *= 1.0f - 0.55f * overcast;
        state.ambientColor *= 1.0f - 0.25f * overcast;
        state.zenithColor = grayed(state.zenithColor);
        state.horizonColor = grayed(state.horizonColor);
        state.horizonFarColor = grayed(state.horizonFarColor);
        state.sunDiscIntensity *= 1.0f - 0.8f * overcast;
    }

    // Weather grading, on top of time-of-day and overcast.
    // Warmth first (a color shift only the low sun feels — haze reddens
    // sunsets), then the intensity multipliers, saturation last so gray
    // weathers stay gray whatever warmth asked for.
    if (weather.warmth > 0.0f) {
        const f32 lowSun = 1.0f - glm::smoothstep(0.05f, 0.35f, elevation);
        const Vec3 warmGain = glm::mix(
            Vec3 { 1.0f }, Vec3 { 1.30f, 0.88f, 0.55f },
            glm::clamp(weather.warmth, 0.0f, 1.0f) * lowSun);
        state.sunColor *= warmGain;
        state.glowColor *= warmGain;
        state.horizonColor *= warmGain;
    }
    state.sunColor *= weather.sunIntensity;
    state.glowColor *= weather.sunIntensity;
    state.sunDiscIntensity *= weather.sunIntensity;
    state.ambientColor *= weather.ambientIntensity;
    if (weather.saturation != 1.0f) {
        const auto resaturate = [&](Vec3& c) {
            const f32 luma = glm::dot(c, Vec3 { 0.299f, 0.587f, 0.114f });
            c = glm::mix(Vec3 { luma }, c, weather.saturation);
        };
        resaturate(state.sunColor);
        resaturate(state.glowColor);
        resaturate(state.ambientColor);
        resaturate(state.zenithColor);
        resaturate(state.horizonColor);
        resaturate(state.horizonFarColor);
    }
    return state;
}

void SkySystem::create(rhi::Device& device, ShaderLibrary& shaders) {
    shaders.load(kSkyShader, { { "FrameUbo", 0 } });
    buildPipeline(device, shaders);

    if (device.caps().offscreenTargets && device.caps().hdrFormats) {
        cloudMap = device.createTexture(
            { .width = kCloudMapSize,
              .height = kCloudMapSize,
              .format = rhi::TextureFormat::R16F,
              .filter = rhi::FilterMode::Linear,
              .usage = rhi::TextureUsage_Sampled |
                       rhi::TextureUsage_RenderAttachment },
            nullptr);
        cloudMapFb = device.createFramebuffer(
            { .colorAttachments = { { .texture = cloudMap } } });
        cloudMapSampler = device.createSampler({}); // linear clamp
        cloudMapGroup = device.createBindGroup(
            { .entries = { { .binding = 2,
                             .texture = cloudMap,
                             .sampler = cloudMapSampler } } });
        shaders.load(kCloudBakeShader, { { "FrameUbo", 0 } }, {},
                     "fullscreen");
    }
}

void SkySystem::destroy(rhi::Device& device) {
    device.destroyPipeline(bakePipeline);
    device.destroyBindGroup(cloudMapGroup);
    device.destroySampler(cloudMapSampler);
    device.destroyFramebuffer(cloudMapFb);
    device.destroyTexture(cloudMap);
    device.destroyPipeline(pipeline);
    *this = SkySystem {};
}

void SkySystem::buildPipeline(rhi::Device& device, ShaderLibrary& shaders) {
    shaders.beginWatch();
    if (pipeline.id != 0) {
        device.destroyPipeline(pipeline);
    }
    // Fullscreen triangle from gl_VertexID: no vertex buffers at all. Depth
    // GreaterEqual against the reversed far-plane clear (0.0), no write:
    // sky pixels are exactly those the opaque pass left untouched.
    pipeline = device.createPipeline(
        { .shader = shaders.get(kSkyShader),
          .depth = { .testEnable = true,
                     .writeEnable = false,
                     .compare = rhi::CompareFunc::GreaterEqual } });
    shaderWatch = shaders.endWatch();
}

void SkySystem::refreshPipeline(rhi::Device& device, ShaderLibrary& shaders) {
    if (shaderWatch.changed(shaders)) {
        buildPipeline(device, shaders);
    }
    if (cloudMapFb.id != 0 &&
        (bakePipeline.id == 0 || bakeWatch.changed(shaders))) {
        if (bakePipeline.id != 0) {
            device.destroyPipeline(bakePipeline);
        }
        shaders.beginWatch();
        bakePipeline = device.createPipeline(
            { .shader = shaders.get(kCloudBakeShader) });
        bakeWatch = shaders.endWatch();
    }
}

void SkySystem::bakeCloudMap(rhi::CommandBuffer& cmd,
                             rhi::BindGroupHandle frameBindGroup) {
    if (cloudMapFb.id == 0 || bakePipeline.id == 0) {
        return;
    }
    cmd.beginRenderPass({ .framebuffer = cloudMapFb,
                          .loadOp = rhi::LoadOp::DontCare,
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.setPipeline(bakePipeline);
    cmd.setBindGroup(0, frameBindGroup);
    cmd.draw(3);
    cmd.endRenderPass();
}

void SkySystem::draw(rhi::CommandBuffer& cmd,
                     rhi::BindGroupHandle frameBindGroup) {
    cmd.setPipeline(pipeline);
    cmd.setBindGroup(0, frameBindGroup);
    cmd.draw(3);
}

} // namespace render
