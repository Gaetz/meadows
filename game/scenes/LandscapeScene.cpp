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
    sky.create(device, *shaders);

    if (device.caps().offscreenTargets) {
        blitSampler = device.createSampler({}); // linear, clamp — identity
        shaders->load(kTonemapShader, { { "FrameUbo", 0 } },
                      { { "uSceneColor", 0 } });
        rebuildBlitPipeline(device);
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
    sky.destroy(device);
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
    blitBindGroup = device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = offscreenColor,
                         .sampler = blitSampler } } });
    offscreenWidth = width;
    offscreenHeight = height;
}

void LandscapeScene::destroyOffscreenTarget(rhi::Device& device) {
    if (offscreenFb.id == 0) {
        return;
    }
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
    sky.refreshPipeline(frame.device, *shaders);
    terrain.setWireframe(wireframeUi, frame.device, *shaders);
    if (regenerateRequested) {
        regenerateRequested = false;
        terrain.regenerate(frame.device);
        grass.regenerate(frame.device);
    }
    terrain.update(frame.device, flyCamera.camera.position);
    grass.update(frame.device, terrain.params, flyCamera.camera.position);

    const render::Camera3D& camera = flyCamera.camera;
    const Mat4 viewProj = camera.viewProj(frame.aspect);
    const render::SkySystem::SkyState skyState = sky.evaluate();
    const render::FrameUniforms uniforms {
        .viewProj = viewProj,
        .invViewProj = glm::inverse(viewProj),
        .cameraPos = { camera.position, 1.0f },
        .time = { timeSeconds, 0.0f, 0.0f, 0.0f },
        .sunDirection = { skyState.sunDirection, 0.0f },
        .sunColor = { skyState.sunColor, skyState.sunDiscIntensity },
        .sunGlowColor = { skyState.glowColor, 0.0f },
        .ambientColor = { skyState.ambientColor, 0.0f },
        .zenithColor = { skyState.zenithColor, 0.0f },
        .horizonColor = { skyState.horizonColor, 0.0f },
        .horizonFarColor = { skyState.horizonFarColor, 0.0f },
        .terrainInfo = { terrain.params.seaLevel, 110.0f, 0.25f, 0.0f },
        .postInfo = { tonemapUi ? 1.0f : 0.0f, exposureUi, 0.0f, 0.0f },
        .fogInfo = { fogDensityUi, fogHeightFalloffUi, fogLowBoostUi,
                     fogStartUi },
    };
    frame.device.updateBuffer(frameUbo, &uniforms, sizeof(uniforms), 0);

    const bool useOffscreen = frame.device.caps().offscreenTargets;
    if (useOffscreen) {
        ensureOffscreenTarget(frame.device, frame.width, frame.height);
        if (shaders->generation(kTonemapShader) != blitShaderGeneration) {
            rebuildBlitPipeline(frame.device);
        }
    }

    // The sky covers every background pixel — no color clear needed.
    frame.cmd.beginRenderPass(
        { .framebuffer = useOffscreen ? offscreenFb : rhi::FramebufferHandle {},
          .loadOp = rhi::LoadOp::DontCare,
          .depthLoadOp = rhi::LoadOp::Clear });
    terrain.draw(frame.cmd, frameBindGroup);
    grass.draw(frame.cmd, frameBindGroup);
    sky.draw(frame.cmd, frameBindGroup); // after opaque: background only
    frame.cmd.endRenderPass();

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
    ImGui::TextUnformatted("Brick 14: animated grass.");
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
    ImGui::Text("Grass tufts: %u", grass.instanceTotal());
    ImGui::InputScalar("Seed", ImGuiDataType_U32, &terrain.params.seed);
    ImGui::SameLine();
    if (ImGui::Button("Regenerate")) {
        regenerateRequested = true; // applied at the top of the next render
    }
    ImGui::Checkbox("Wireframe (LOD debug)", &wireframeUi);
    ImGui::Separator();
    ImGui::SliderFloat("Time of day (h)", &sky.timeOfDay, 0.0f, 24.0f,
                       "%.1f");
    ImGui::Checkbox("Animate (24 h in 2 min)", &animateTime);
    ImGui::Separator();
    ImGui::Checkbox("Filmic tonemap (A/B)", &tonemapUi);
    ImGui::SliderFloat("Exposure", &exposureUi, 0.25f, 3.0f, "%.2f");
    ImGui::SliderFloat("Fog density", &fogDensityUi, 0.0f, 0.004f, "%.4f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Fog height falloff", &fogHeightFalloffUi, 0.001f,
                       0.08f, "%.3f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Fog low-altitude boost", &fogLowBoostUi, 0.0f, 5.0f,
                       "%.1f");
    ImGui::SliderFloat("Fog start (m)", &fogStartUi, 0.0f, 500.0f, "%.0f");
    ImGui::End();
}

} // namespace game
