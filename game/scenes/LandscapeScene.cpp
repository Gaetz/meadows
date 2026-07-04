#include "game/scenes/LandscapeScene.hpp"

#include <glm/glm.hpp>
#include <imgui.h>

#include "engine/Engine.hpp"
#include "engine/FrameContext.hpp"
#include "engine/platform/Window.hpp"
#include "engine/render/landscape/FrameUniforms.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace game {

namespace {

constexpr const char* kTonemapShader = "tonemap";

// Lengyel's oblique near plane: bends the projection's near plane onto an
// arbitrary view-space plane, so the mirrored render clips everything below
// the water for free (no user clip distance in the shaders).
Mat4 obliqueProjection(Mat4 proj, const Vec4& clipPlaneView) {
    Vec4 q;
    q.x = (glm::sign(clipPlaneView.x) + proj[2][0]) / proj[0][0];
    q.y = (glm::sign(clipPlaneView.y) + proj[2][1]) / proj[1][1];
    q.z = -1.0f;
    q.w = (1.0f + proj[2][2]) / proj[3][2];
    const Vec4 c = clipPlaneView * (2.0f / glm::dot(clipPlaneView, q));
    proj[0][2] = c.x;
    proj[1][2] = c.y;
    proj[2][2] = c.z + 1.0f;
    proj[3][2] = c.w;
    return proj;
}

} // namespace

void LandscapeScene::onEnter() {
    rhi::Device& device = engine->getDevice();

    frameUbo = device.createBuffer({ .usage = rhi::BufferUsage::Uniform,
                                     .size = sizeof(render::FrameUniforms),
                                     .dynamic = true },
                                   nullptr);
    frameBindGroup = device.createBindGroup(
        { .entries = { { .binding = 0, .buffer = frameUbo } } });

    shaders = std::make_unique<render::ShaderLibrary>(device);
    terrain.create(device, *shaders, engine->getJobSystem());
    grass.create(device, *shaders, engine->getJobSystem());
    vegetation.create(device, *shaders, engine->getJobSystem(),
                      terrain.params.seed);
    sky.create(device, *shaders);
    if (device.caps().offscreenTargets && device.caps().textureArrays) {
        shadows.create(device);
    }
    if (device.caps().copyTexture) {
        water.create(device, *shaders);
        depthSampler = device.createSampler(
            { .minFilter = rhi::FilterMode::Nearest,
              .magFilter = rhi::FilterMode::Nearest });
        reflectionUbo =
            device.createBuffer({ .usage = rhi::BufferUsage::Uniform,
                                  .size = sizeof(render::FrameUniforms),
                                  .dynamic = true },
                                nullptr);
        reflectionBindGroup = device.createBindGroup(
            { .entries = { { .binding = 0, .buffer = reflectionUbo } } });
    }

    if (device.caps().offscreenTargets) {
        blitSampler = device.createSampler({}); // linear, clamp — identity
        shaders->load(kTonemapShader, { { "FrameUbo", 0 } },
                      { { "uSceneColor", 0 },
                        { "uBloom", 1 },
                        { "uGodRays", 2 },
                        { "uVolumetric", 3 } });
        rebuildBlitPipeline(device);
    }
    if (device.caps().offscreenTargets && device.caps().hdrFormats &&
        device.caps().copyTexture) {
        postFx.create(device, *shaders);
    }

    flyCamera.camera.position = { 32.0f, 110.0f, 400.0f };
    flyCamera.camera.pitch = -0.30f;
    // Cover the full streamed ring (~14 chunks = ~900 m) plus headroom.
    flyCamera.camera.farPlane = 1600.0f;
}

void LandscapeScene::onExit() {
    rhi::Device& device = engine->getDevice();
    engine->getWindow().setRelativeMouseMode(false);
    destroyOffscreenTarget(device);
    device.destroyPipeline(blitPipeline);
    device.destroySampler(blitSampler);
    postFx.destroy(device);
    water.destroy(device);
    device.destroyBindGroup(reflectionBindGroup);
    device.destroyBuffer(reflectionUbo);
    device.destroySampler(depthSampler);
    shadows.destroy(device);
    sky.destroy(device);
    vegetation.destroy(device);
    grass.destroy(device);
    terrain.destroy(device);
    shaders.reset(); // destroys the library's shader programs
    device.destroyBindGroup(frameBindGroup);
    device.destroyBuffer(frameUbo);
}

void LandscapeScene::ensureOffscreenTarget(rhi::Device& device, u32 width,
                                           u32 height) {
    if (offscreenFb.id != 0 && offscreenWidth == width &&
        offscreenHeight == height) {
        return;
    }
    destroyOffscreenTarget(device);
    // HDR scene target: the sky/sun palette is linear HDR (sun > 1); the
    // tonemap pass compresses to display range.
    offscreenColor = device.createTexture(
        { .width = width,
          .height = height,
          .format = device.caps().hdrFormats ? rhi::TextureFormat::RGBA16F
                                             : rhi::TextureFormat::RGBA8,
          .filter = rhi::FilterMode::Linear,
          .usage = rhi::TextureUsage_Sampled |
                   rhi::TextureUsage_RenderAttachment },
        nullptr);
    offscreenDepth = device.createTexture(
        { .width = width,
          .height = height,
          .format = rhi::TextureFormat::Depth32F,
          .usage = rhi::TextureUsage_RenderAttachment },
        nullptr);
    offscreenFb = device.createFramebuffer(
        { .colorAttachments = { { .texture = offscreenColor } },
          .depthAttachment = { .texture = offscreenDepth } });
    if (device.caps().copyTexture) {
        sceneColorCopy = device.createTexture(
            { .width = width,
              .height = height,
              .format = device.caps().hdrFormats ? rhi::TextureFormat::RGBA16F
                                                 : rhi::TextureFormat::RGBA8,
              .filter = rhi::FilterMode::Linear,
              .usage = rhi::TextureUsage_Sampled },
            nullptr);
        sceneDepthCopy = device.createTexture(
            { .width = width,
              .height = height,
              .format = rhi::TextureFormat::Depth32F,
              .usage = rhi::TextureUsage_Sampled },
            nullptr);
        const u32 reflectionWidth = glm::max(width / 2, 1u);
        const u32 reflectionHeight = glm::max(height / 2, 1u);
        reflectionColor = device.createTexture(
            { .width = reflectionWidth,
              .height = reflectionHeight,
              .format = device.caps().hdrFormats ? rhi::TextureFormat::RGBA16F
                                                 : rhi::TextureFormat::RGBA8,
              .filter = rhi::FilterMode::Linear,
              .usage = rhi::TextureUsage_Sampled |
                       rhi::TextureUsage_RenderAttachment },
            nullptr);
        reflectionDepth = device.createTexture(
            { .width = reflectionWidth,
              .height = reflectionHeight,
              .format = rhi::TextureFormat::Depth32F,
              .usage = rhi::TextureUsage_RenderAttachment },
            nullptr);
        reflectionFb = device.createFramebuffer(
            { .colorAttachments = { { .texture = reflectionColor } },
              .depthAttachment = { .texture = reflectionDepth } });
        waterSceneBindGroup = device.createBindGroup(
            { .entries = { { .binding = 0,
                             .texture = sceneColorCopy,
                             .sampler = blitSampler },
                           { .binding = 1,
                             .texture = sceneDepthCopy,
                             .sampler = depthSampler },
                           { .binding = 2,
                             .texture = reflectionColor,
                             .sampler = blitSampler } } });
    }

    if (device.caps().offscreenTargets && device.caps().hdrFormats &&
        device.caps().copyTexture) {
        postFx.resize(device, width, height, offscreenColor, sceneColorCopy,
                      sceneDepthCopy);
    }
    // Tonemap inputs: scene + bloom + god rays (black 1x1 fallbacks are not
    // needed on the 4.6 path — postFx is always ready when we get here).
    blitBindGroup = device.createBindGroup(
        { .entries =
              postFx.ready()
                  ? vector<rhi::BindGroupEntry> {
                        { .binding = 0,
                          .texture = offscreenColor,
                          .sampler = blitSampler },
                        { .binding = 1,
                          .texture = postFx.bloomTexture(),
                          .sampler = blitSampler },
                        { .binding = 2,
                          .texture = postFx.godRayTexture(),
                          .sampler = blitSampler },
                        { .binding = 3,
                          .texture = postFx.volumetricTexture(),
                          .sampler = blitSampler } }
                  : vector<rhi::BindGroupEntry> {
                        { .binding = 0,
                          .texture = offscreenColor,
                          .sampler = blitSampler } } });
    offscreenWidth = width;
    offscreenHeight = height;
}

void LandscapeScene::destroyOffscreenTarget(rhi::Device& device) {
    if (offscreenFb.id == 0) {
        return;
    }
    device.destroyBindGroup(waterSceneBindGroup);
    device.destroyFramebuffer(reflectionFb);
    device.destroyTexture(reflectionDepth);
    device.destroyTexture(reflectionColor);
    device.destroyTexture(sceneDepthCopy);
    device.destroyTexture(sceneColorCopy);
    waterSceneBindGroup = {};
    reflectionFb = {};
    reflectionDepth = {};
    reflectionColor = {};
    sceneDepthCopy = {};
    sceneColorCopy = {};
    device.destroyBindGroup(blitBindGroup);
    device.destroyFramebuffer(offscreenFb);
    device.destroyTexture(offscreenDepth);
    device.destroyTexture(offscreenColor);
    blitBindGroup = {};
    offscreenFb = {};
    offscreenDepth = {};
    offscreenColor = {};
    offscreenWidth = 0;
    offscreenHeight = 0;
}

void LandscapeScene::rebuildBlitPipeline(rhi::Device& device) {
    if (blitPipeline.id != 0) {
        device.destroyPipeline(blitPipeline);
    }
    blitPipeline =
        device.createPipeline({ .shader = shaders->get(kTonemapShader) });
    blitShaderGeneration = shaders->generation(kTonemapShader);
}

void LandscapeScene::update(f32 dt) {
    timeSeconds += dt;
    // Don't steal the mouse from ImGui: clicking a panel must not mouselook.
    const bool allowCapture = !ImGui::GetIO().WantCaptureMouse;
    flyCamera.update(engine->getInput(), engine->getWindow(), dt,
                     allowCapture);
    if (animateTime) {
        // Full day/night cycle in two minutes.
        sky.timeOfDay += dt * (24.0f / 120.0f);
        if (sky.timeOfDay >= 24.0f) {
            sky.timeOfDay -= 24.0f;
        }
    }
}

void LandscapeScene::render(engine::FrameContext& frame) {
    shaders->pollHotReload(frame.dt);
    terrain.refreshPipeline(frame.device, *shaders);
    grass.refreshPipeline(frame.device, *shaders);
    vegetation.refreshPipeline(frame.device, *shaders);
    sky.refreshPipeline(frame.device, *shaders);
    if (frame.device.caps().copyTexture) {
        water.refreshPipeline(frame.device, *shaders);
    }
    postFx.refreshPipelines(frame.device, *shaders);
    terrain.setWireframe(wireframeUi, frame.device, *shaders);
    if (regenerateRequested) {
        regenerateRequested = false;
        terrain.regenerate(frame.device);
        grass.regenerate(frame.device);
        vegetation.regenerate(frame.device, terrain.params.seed);
    }
    terrain.update(frame.device, flyCamera.camera.position);
    grass.update(frame.device, terrain.params, flyCamera.camera.position);
    vegetation.update(frame.device, terrain.params,
                      flyCamera.camera.position);

    const render::Camera3D& camera = flyCamera.camera;
    const Mat4 viewProj = camera.viewProj(frame.aspect);
    const render::SkySystem::SkyState skyState =
        sky.evaluate(cloudCoverageUi);

    // Shadows ramp out as the sun crosses the horizon (no sun, no shadows),
    // and soften away under heavy cloud cover (diffuse light casts none).
    const bool shadowsAvailable = shadows.receiverBindGroup().id != 0;
    const f32 shadowStrength =
        (shadowsUi && shadowsAvailable)
            ? glm::smoothstep(-0.02f, 0.06f, skyState.sunDirection.y) *
                  (1.0f - 0.65f * cloudCoverageUi)
            : 0.0f;
    render::ShadowMapper::Cascades cascades {};
    if (shadowStrength > 0.0f) {
        cascades = shadows.computeCascades(camera, frame.aspect,
                                           skyState.sunDirection);
        shadows.updateCascadeUbos(frame.device, cascades);
    }

    // Planar reflection is meaningful only from above the surface.
    const bool reflectionsActive =
        reflectionsUi && reflectionFb.id != 0 &&
        camera.position.y > terrain.params.seaLevel;

    // Sun position on screen for the god rays; shafts fade as the sun
    // leaves the frame or dips below the horizon.
    Vec2 sunUv { 0.5f, 0.5f };
    f32 shaftFade = 0.0f;
    {
        const Vec4 clip =
            viewProj *
            Vec4 { camera.position + skyState.sunDirection * 1000.0f, 1.0f };
        if (clip.w > 0.0f) {
            const Vec2 ndc { clip.x / clip.w, clip.y / clip.w };
            sunUv = ndc * 0.5f + Vec2 { 0.5f };
            const f32 edge = glm::max(std::abs(ndc.x), std::abs(ndc.y));
            shaftFade =
                (1.0f - glm::smoothstep(0.85f, 1.35f, edge)) *
                glm::smoothstep(-0.02f, 0.05f, skyState.sunDirection.y);
        }
    }

    const render::FrameUniforms uniforms {
        .viewProj = viewProj,
        .invViewProj = glm::inverse(viewProj),
        .cameraPos = { camera.position, 1.0f },
        .time = { timeSeconds, 0.0f, volumetricUi,
                  static_cast<f32>(debugBufferUi) },
        .sunDirection = { skyState.sunDirection, 0.0f },
        .sunColor = { skyState.sunColor, skyState.sunDiscIntensity },
        .sunGlowColor = { skyState.glowColor, 0.0f },
        .ambientColor = { skyState.ambientColor, 0.0f },
        .zenithColor = { skyState.zenithColor, 0.0f },
        .horizonColor = { skyState.horizonColor, 0.0f },
        .horizonFarColor = { skyState.horizonFarColor, 0.0f },
        .terrainInfo = { terrain.params.seaLevel, 110.0f, 0.25f,
                         reflectionsActive ? 1.0f : 0.0f },
        .postInfo = { tonemapUi ? 1.0f : 0.0f, exposureUi,
                      cascadeDebugUi ? 1.0f : 0.0f, bloomIntensityUi },
        .fogInfo = { fogDensityUi, fogHeightFalloffUi, fogLowBoostUi,
                     fogStartUi },
        .sunViewProj = cascades.viewProj,
        .cascadeSplits = { cascades.splitFar[0], cascades.splitFar[1],
                           cascades.splitFar[2], 0.0f },
        .shadowInfo = { cascades.texelWorld[0], cascades.texelWorld[1],
                        cascades.texelWorld[2], shadowStrength },
        .screenInfo = { static_cast<f32>(frame.width),
                        static_cast<f32>(frame.height),
                        1.0f / static_cast<f32>(frame.width),
                        1.0f / static_cast<f32>(frame.height) },
        .cloudInfo = { cloudCoverageUi, 520.0f, 0.0011f, cloudShadowUi },
        .sunScreen = { sunUv.x, sunUv.y, shaftFade, godRayIntensityUi },
    };
    frame.device.updateBuffer(frameUbo, &uniforms, sizeof(uniforms), 0);

    // Cascade passes: depth-only casters from the sun's point of view.
    if (shadowStrength > 0.0f) {
        for (u32 i = 0; i < render::ShadowMapper::kCascadeCount; ++i) {
            frame.cmd.beginRenderPass(
                { .framebuffer = shadows.framebuffer(i),
                  .loadOp = rhi::LoadOp::DontCare,
                  .depthLoadOp = rhi::LoadOp::Clear });
            terrain.drawDepth(frame.cmd, shadows.casterBindGroup(i),
                              camera.position, 9);
            vegetation.drawDepth(frame.cmd, frameBindGroup,
                                 shadows.casterBindGroup(i));
            frame.cmd.endRenderPass();
        }
    }

    const bool useOffscreen = frame.device.caps().offscreenTargets;
    if (useOffscreen) {
        ensureOffscreenTarget(frame.device, frame.width, frame.height);
        if (shaders->generation(kTonemapShader) != blitShaderGeneration) {
            rebuildBlitPipeline(frame.device);
        }
    }

    // Planar reflection: the scene mirrored about the water plane, at half
    // resolution. The mirrored view flips triangle winding (front face CW),
    // and an oblique near plane clips everything below the surface.
    if (reflectionsActive) {
        const f32 waterY = terrain.params.seaLevel;
        Mat4 mirror { 1.0f };
        mirror[1][1] = -1.0f;
        mirror[3][1] = 2.0f * waterY;
        const Mat4 reflectedView = camera.view() * mirror;
        // Keep the above-water side; tiny epsilon avoids a clipped seam
        // right at the waterline.
        const Vec4 planeWorld { 0.0f, 1.0f, 0.0f, -(waterY - 0.08f) };
        const Vec4 planeView =
            glm::transpose(glm::inverse(reflectedView)) * planeWorld;
        const Mat4 reflectedProj =
            obliqueProjection(camera.proj(frame.aspect), planeView);
        const Mat4 reflectedViewProj = reflectedProj * reflectedView;

        render::FrameUniforms reflectionUniforms = uniforms;
        reflectionUniforms.viewProj = reflectedViewProj;
        reflectionUniforms.invViewProj = glm::inverse(reflectedViewProj);
        reflectionUniforms.cameraPos = { camera.position.x,
                                         2.0f * waterY - camera.position.y,
                                         camera.position.z, 1.0f };
        frame.device.updateBuffer(reflectionUbo, &reflectionUniforms,
                                  sizeof(reflectionUniforms), 0);

        frame.cmd.beginRenderPass({ .framebuffer = reflectionFb,
                                    .loadOp = rhi::LoadOp::DontCare,
                                    .depthLoadOp = rhi::LoadOp::Clear });
        frame.cmd.setFrontFace(rhi::FrontFace::Clockwise);
        terrain.draw(frame.cmd, reflectionBindGroup,
                     shadows.receiverBindGroup());
        vegetation.draw(frame.cmd, reflectionBindGroup,
                        shadows.receiverBindGroup());
        sky.draw(frame.cmd, reflectionBindGroup);
        frame.cmd.endRenderPass();
    }

    // The sky covers every background pixel — no color clear needed.
    frame.cmd.beginRenderPass(
        { .framebuffer = useOffscreen ? offscreenFb : rhi::FramebufferHandle {},
          .loadOp = rhi::LoadOp::DontCare,
          .depthLoadOp = rhi::LoadOp::Clear });
    terrain.draw(frame.cmd, frameBindGroup, shadows.receiverBindGroup());
    vegetation.draw(frame.cmd, frameBindGroup, shadows.receiverBindGroup());
    grass.draw(frame.cmd, frameBindGroup, shadows.receiverBindGroup());
    sky.draw(frame.cmd, frameBindGroup); // after opaque: background only
    frame.cmd.endRenderPass();

    // Water: snapshot the opaque scene (sampling a bound attachment is UB),
    // then compose refraction/reflection/foam back into the HDR target.
    if (useOffscreen && frame.device.caps().copyTexture &&
        waterSceneBindGroup.id != 0) {
        frame.cmd.copyTexture(offscreenColor, sceneColorCopy);
        frame.cmd.copyTexture(offscreenDepth, sceneDepthCopy);
        frame.cmd.beginRenderPass({ .framebuffer = offscreenFb,
                                    .loadOp = rhi::LoadOp::Load,
                                    .depthLoadOp = rhi::LoadOp::Load });
        water.draw(frame.cmd, frameBindGroup, waterSceneBindGroup);
        frame.cmd.endRenderPass();
    }

    // Bloom pyramid + god rays + volumetric shafts, composed by the tonemap.
    if (useOffscreen) {
        postFx.render(frame.cmd, frameBindGroup,
                      shadows.receiverBindGroup());
    }

    if (useOffscreen) {
        // Tonemap composite: HDR scene -> filmic curve -> gamma -> backbuffer.
        frame.cmd.beginRenderPass({ .loadOp = rhi::LoadOp::DontCare,
                                    .depthLoadOp = rhi::LoadOp::DontCare });
        frame.cmd.setPipeline(blitPipeline);
        frame.cmd.setBindGroup(0, frameBindGroup); // FrameUbo (uPostInfo)
        frame.cmd.setBindGroup(1, blitBindGroup);  // scene color + sampler
        frame.cmd.draw(3);
        frame.cmd.endRenderPass();
    }
}

void LandscapeScene::drawUi() {
    ImGui::Begin("Landscape", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted("Brick 21: bloom + god rays.");
    ImGui::Text("%.1f FPS (%.2f ms)", ImGui::GetIO().Framerate,
                1000.0f / ImGui::GetIO().Framerate);
    ImGui::TextUnformatted(
        "Hold LMB: mouselook | WASD: move | E/Space: up | Q/Ctrl: down\n"
        "Shift: speed boost");
    const Vec3 p = flyCamera.camera.position;
    ImGui::Text("Position: %.1f  %.1f  %.1f", p.x, p.y, p.z);
    ImGui::SliderFloat("Move speed (m/s)", &flyCamera.moveSpeed, 2.0f, 150.0f,
                       "%.0f", ImGuiSliderFlags_Logarithmic);
    ImGui::Separator();
    ImGui::Text("Resident: %u | pending: %u | uploads/frame: %u",
                terrain.residentCount(), terrain.pendingCount(),
                terrain.uploadsLastFrame());
    ImGui::Text("Grass blades: %u | props: %u", grass.instanceTotal(),
                vegetation.propTotal());
    ImGui::InputScalar("Seed", ImGuiDataType_U32, &terrain.params.seed);
    ImGui::SameLine();
    if (ImGui::Button("Regenerate")) {
        regenerateRequested = true; // applied at the top of the next render
    }
    // Water plane, sand band and material weights follow live; the scatter
    // (grass/trees/props) is baked per chunk — Regenerate to re-align it.
    ImGui::SliderFloat("Sea level (m)", &terrain.params.seaLevel, 0.0f, 40.0f,
                       "%.0f");
    ImGui::Checkbox("Wireframe (LOD debug)", &wireframeUi);
    ImGui::Separator();
    ImGui::SliderFloat("Time of day (h)", &sky.timeOfDay, 0.0f, 24.0f,
                       "%.1f");
    ImGui::Checkbox("Animate (24 h in 2 min)", &animateTime);
    ImGui::Separator();
    ImGui::Checkbox("Filmic tonemap (A/B)", &tonemapUi);
    ImGui::SliderFloat("Bloom intensity", &bloomIntensityUi, 0.0f, 1.5f,
                       "%.2f");
    ImGui::SliderFloat("God rays intensity", &godRayIntensityUi, 0.0f, 2.0f,
                       "%.2f");
    ImGui::SliderFloat("Volumetric shafts", &volumetricUi, 0.0f, 3.0f,
                       "%.2f");
    ImGui::Combo("Debug buffer", &debugBufferUi,
                 "Off\0Bloom\0God rays\0Volumetric\0");
    ImGui::Checkbox("Shadows", &shadowsUi);
    ImGui::SameLine();
    ImGui::Checkbox("Cascade debug tint", &cascadeDebugUi);
    ImGui::Checkbox("Water reflections", &reflectionsUi);
    ImGui::SliderFloat("Exposure", &exposureUi, 0.25f, 3.0f, "%.2f");
    ImGui::SliderFloat("Fog density", &fogDensityUi, 0.0f, 0.004f, "%.4f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Fog height falloff", &fogHeightFalloffUi, 0.001f,
                       0.08f, "%.3f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Fog low-altitude boost", &fogLowBoostUi, 0.0f, 5.0f,
                       "%.1f");
    ImGui::SliderFloat("Fog start (m)", &fogStartUi, 0.0f, 500.0f, "%.0f");
    ImGui::SliderFloat("Cloud coverage", &cloudCoverageUi, 0.0f, 1.0f,
                       "%.2f");
    ImGui::SliderFloat("Cloud shadow strength", &cloudShadowUi, 0.0f, 1.0f,
                       "%.2f");
    ImGui::End();
}

} // namespace game
