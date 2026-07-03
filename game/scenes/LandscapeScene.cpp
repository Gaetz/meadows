#include "game/scenes/LandscapeScene.hpp"

#include <imgui.h>

#include "engine/FrameContext.hpp"
#include "engine/rhi/CommandBuffer.hpp"

namespace game {

namespace {
// Placeholder daylight sky until the SkySystem paints a real gradient.
constexpr rhi::Color kSkyBlue { 0.45f, 0.71f, 0.95f, 1.0f };
} // namespace

void LandscapeScene::render(engine::FrameContext& frame) {
    frame.cmd.beginRenderPass({ .loadOp = rhi::LoadOp::Clear,
                                .clearColor = kSkyBlue });
    frame.cmd.endRenderPass();
}

void LandscapeScene::drawUi() {
    ImGui::Begin("Landscape", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted("Brick 1: frame seam - sky clear only.");
    ImGui::End();
}

} // namespace game
