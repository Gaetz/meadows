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
    sky.create(device, *shaders);

    flyCamera.camera.position = { 32.0f, 110.0f, 400.0f };
    flyCamera.camera.pitch = -0.30f;
    // Cover the full streamed ring (~14 chunks = ~900 m) plus headroom.
    flyCamera.camera.farPlane = 1600.0f;
}

void LandscapeScene::onExit() {
    rhi::Device& device = engine->getDevice();
    engine->getWindow().setRelativeMouseMode(false);
    sky.destroy(device);
    terrain.destroy(device);
    shaders.reset(); // destroys the library's shader programs
    device.destroyBindGroup(frameBindGroup);
    device.destroyBuffer(frameUbo);
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
    sky.refreshPipeline(frame.device, *shaders);
    terrain.setWireframe(wireframeUi, frame.device, *shaders);
    if (regenerateRequested) {
        regenerateRequested = false;
        terrain.regenerate(frame.device);
    }
    terrain.update(frame.device, flyCamera.camera.position);

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
    };
    frame.device.updateBuffer(frameUbo, &uniforms, sizeof(uniforms), 0);

    // The sky covers every background pixel — no color clear needed.
    frame.cmd.beginRenderPass({ .loadOp = rhi::LoadOp::DontCare,
                                .depthLoadOp = rhi::LoadOp::Clear });
    terrain.draw(frame.cmd, frameBindGroup);
    sky.draw(frame.cmd, frameBindGroup); // after opaque: background only
    frame.cmd.endRenderPass();
}

void LandscapeScene::drawUi() {
    ImGui::Begin("Landscape", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted("Brick 9: sky + day/night cycle.");
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
    ImGui::End();
}

} // namespace game
