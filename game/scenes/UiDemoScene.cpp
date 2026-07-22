#include "game/scenes/UiDemoScene.hpp"

#include <imgui.h>

#include "engine/Engine.hpp"
#include "engine/FrameContext.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Input.hpp"
#include "engine/platform/Paths.hpp"
#include "engine/platform/Window.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace game {

void UiDemoScene::onEnter() {
    rhi::Device& device = engine->getDevice();
    shaders = std::make_unique<render::ShaderLibrary>(device);

    // Document roots in plugin-stack order (last wins). One root today;
    // the interfaces vertical feeds every plugin's ui/ dir here.
    const auto uiRoot = platform::executableDir() / "data" / "base" / "ui";
    lastWidth = engine->getWindow().width();
    lastHeight = engine->getWindow().height();
    if (!uiSystem.create(device, *shaders, { uiRoot }, lastWidth,
                         lastHeight)) {
        statusLine = "UiSystem creation FAILED (see log)";
        return;
    }
    if (!uiSystem.loadFont(uiRoot / "fonts" / "DemoFont.ttf")) {
        statusLine = "font load failed: " +
                     (uiRoot / "fonts" / "DemoFont.ttf").string();
        return;
    }
    documentShown = uiSystem.showDocument("hello.rml");
    statusLine = documentShown ? "hello.rml loaded from the plugin ui/ root"
                               : "hello.rml FAILED to load";
    audioSystem.create();
}

void UiDemoScene::onExit() {
    audioSystem.destroy();
    uiSystem.destroy(engine->getDevice());
    shaders.reset();
}

void UiDemoScene::update(f32 dt) {
    const platform::Input& input = engine->getInput();
    const Vec2 mouse = input.mousePosition();
    uiSystem.processMouseMove(static_cast<i32>(mouse.x),
                              static_cast<i32>(mouse.y));
    uiSystem.update(dt);
    audioSystem.update(dt);
}

void UiDemoScene::render(engine::FrameContext& frame) {
    shaders->pollHotReload(frame.dt);
    if (frame.width != lastWidth || frame.height != lastHeight) {
        lastWidth = frame.width;
        lastHeight = frame.height;
        uiSystem.resize(lastWidth, lastHeight);
    }
    frame.cmd.beginRenderPass(
        { .loadOp = rhi::LoadOp::Clear,
          .clearColor = { 0.10f, 0.12f, 0.10f, 1.0f } });
    uiSystem.render(frame.cmd, frame.device, frame.width, frame.height);
    frame.cmd.endRenderPass();
}

void UiDemoScene::drawUi() {
    ImGui::Begin("UI demo", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted("H4: RmlUi through the RHI + plugin VFS roots.");
    ImGui::TextUnformatted(statusLine.c_str());
    ImGui::TextUnformatted(
        "Documents hot-edit: change data/base/ui/hello.rcss and reload.");
    if (ImGui::Button("Reload document")) {
        uiSystem.closeDocuments();
        documentShown = uiSystem.showDocument("hello.rml");
    }
    ImGui::SameLine();
    if (ImGui::Button("Play test tone")) {
        audioSystem.playTestTone(0.3f, 440.0f); // the audible proof
    }
    ImGui::End();
}

} // namespace game
