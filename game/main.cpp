#include <algorithm>
#include <cmath>

#include <glm/gtc/constants.hpp>
#include <imgui.h>

#include "engine/Engine.hpp"
#include "engine/Game.hpp"
#include "engine/core/Defines.hpp"
#include "engine/render/SpriteRenderer.hpp"
#include "engine/rhi/Device.hpp"

namespace {

// Procedural textures stand in for real assets until the asset DB (Phase 1).
// Pixel layout is 0xAABBGGRR (RGBA8, little endian).

rhi::TextureHandle createCheckerTexture(rhi::Device& device) {
    constexpr u32 size = 64;
    constexpr u32 cell = 8;
    vector<u32> pixels(size * size);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const bool even = ((x / cell + y / cell) % 2) == 0;
            pixels[y * size + x] = even ? 0xFFFFFFFF : 0xFFC8C8C8;
        }
    }
    return device.createTexture({ .width = size, .height = size },
                                pixels.data());
}

rhi::TextureHandle createDiscTexture(rhi::Device& device) {
    constexpr u32 size = 64;
    vector<u32> pixels(size * size);
    const f32 center = (size - 1) * 0.5f;
    const f32 radius = size * 0.45f;
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const f32 dx = static_cast<f32>(x) - center;
            const f32 dy = static_cast<f32>(y) - center;
            const f32 dist = std::sqrt(dx * dx + dy * dy);
            // 2-pixel soft edge so alpha blending is visible.
            const f32 alpha =
                std::clamp((radius - dist) * 0.5f + 0.5f, 0.0f, 1.0f);
            pixels[y * size + x] =
                (static_cast<u32>(alpha * 255.0f) << 24) | 0x00FFFFFF;
        }
    }
    return device.createTexture(
        { .width = size, .height = size, .filter = rhi::FilterMode::Linear },
        pixels.data());
}

class TrueAdventurer final : public engine::Game {
public:
    void init(engine::Engine& engine) override {
        checker = createCheckerTexture(engine.getDevice());
        disc = createDiscTexture(engine.getDevice());
        engine.getCamera().viewHeight = 12.0f;
    }

    void update(f32 dt) override {
        time += dt * speed;
    }

    void draw(render::SpriteRenderer& renderer) override {
        // Meadow ground: one tinted checker tile per world unit.
        for (i32 y = -6; y < 6; ++y) {
            for (i32 x = -10; x < 10; ++x) {
                const f32 shade =
                    0.85f +
                    0.15f * static_cast<f32>((x * 7 + y * 13 + 60) % 5) / 4.0f;
                renderer.draw({
                    .position = { static_cast<f32>(x) + 0.5f,
                                  static_cast<f32>(y) + 0.5f },
                    .tint = { 0.35f * shade, 0.55f * shade, 0.30f * shade,
                              1.0f },
                    .texture = checker,
                });
            }
        }

        // Rotating ring of translucent discs over the ground.
        for (u32 k = 0; k < discCount; ++k) {
            const f32 angle = time * 0.5f + static_cast<f32>(k) *
                                                glm::two_pi<f32>() /
                                                static_cast<f32>(discCount);
            const f32 hue = static_cast<f32>(k) / static_cast<f32>(discCount);
            renderer.draw({
                .position = { ringRadius * std::cos(angle),
                              ringRadius * std::sin(angle) },
                .size = { 1.5f, 1.5f },
                .rotation = time,
                .tint = { 0.5f + 0.5f * std::cos(hue * glm::two_pi<f32>()),
                          0.5f +
                              0.5f * std::cos((hue + 0.33f) * glm::two_pi<f32>()),
                          0.5f +
                              0.5f * std::cos((hue + 0.67f) * glm::two_pi<f32>()),
                          0.85f },
                .texture = disc,
            });
        }
    }

    void drawUi() override {
        ImGui::Begin("True Adventurer");
        ImGui::Text("%.1f fps (%.2f ms)", ImGui::GetIO().Framerate,
                    1000.0f / ImGui::GetIO().Framerate);
        ImGui::SliderFloat("Ring radius", &ringRadius, 1.0f, 5.0f);
        ImGui::SliderFloat("Speed", &speed, 0.0f, 4.0f);
        int count = static_cast<int>(discCount);
        if (ImGui::SliderInt("Discs", &count, 1, 64)) {
            discCount = static_cast<u32>(count);
        }
        ImGui::End();
    }

    void close() override {}

private:
    rhi::TextureHandle checker {};
    rhi::TextureHandle disc {};
    f32 time { 0.0f };
    f32 speed { 1.0f };
    f32 ringRadius { 4.0f };
    u32 discCount { 8 };
};

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    TrueAdventurer game;
    return engine::Engine::run({ .title = "True Adventurer" }, game);
}
