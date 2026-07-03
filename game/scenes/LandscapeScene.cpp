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
// Placeholder daylight sky until the SkySystem paints a real gradient.
constexpr rhi::Color kSkyBlue { 0.45f, 0.71f, 0.95f, 1.0f };
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

    flyCamera.camera.position = { 32.0f, 110.0f, 400.0f };
    flyCamera.camera.pitch = -0.30f;
    // Cover the full streamed ring (~14 chunks = ~900 m) plus headroom.
    flyCamera.camera.farPlane = 1600.0f;
}

void LandscapeScene::onExit() {
    rhi::Device& device = engine->getDevice();
    engine->getWindow().setRelativeMouseMode(false);
    terrain.destroy(device);
    shaders.reset(); // destroys the library's shader programs
    device.destroyBindGroup(frameBindGroup);
    device.destroyBuffer(frameUbo);
}

void LandscapeScene::update(f32 dt) {
    timeSeconds += dt;
    flyCamera.update(engine->getInput(), engine->getWindow(), dt);
}

void LandscapeScene::render(engine::FrameContext& frame) {
    shaders->pollHotReload(frame.dt);
    terrain.refreshPipeline(frame.device, *shaders);
    terrain.setWireframe(wireframeUi, frame.device, *shaders);
    if (regenerateRequested) {
        regenerateRequested = false;
        terrain.regenerate(frame.device);
    }
    terrain.update(frame.device, flyCamera.camera.position);

    const render::Camera3D& camera = flyCamera.camera;
    const render::FrameUniforms uniforms {
        .viewProj = camera.viewProj(frame.aspect),
        .cameraPos = { camera.position, 1.0f },
        .time = { timeSeconds, 0.0f, 0.0f, 0.0f },
    };
    frame.device.updateBuffer(frameUbo, &uniforms, sizeof(uniforms), 0);

    frame.cmd.beginRenderPass({ .loadOp = rhi::LoadOp::Clear,
                                .clearColor = kSkyBlue,
                                .depthLoadOp = rhi::LoadOp::Clear });
    terrain.draw(frame.cmd, frameBindGroup);
    frame.cmd.endRenderPass();
}

void LandscapeScene::drawUi() {
    ImGui::Begin("Landscape", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted("Brick 8: LOD rings + skirts (~900 m view).");
    ImGui::TextUnformatted(
        "Hold RMB: mouselook | WASD: move | E/Space: up | Q/Ctrl: down\n"
        "Shift: speed boost");
    const Vec3 p = flyCamera.camera.position;
    ImGui::Text("Position: %.1f  %.1f  %.1f", p.x, p.y, p.z);
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
    ImGui::End();
}

} // namespace game
