#include <algorithm>
#include <chrono>
#include <cmath>

#include <glm/gtc/constants.hpp>

#include "engine/core/Defines.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Window.hpp"
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
            const f32 alpha = std::clamp((radius - dist) * 0.5f + 0.5f, 0.0f, 1.0f);
            pixels[y * size + x] =
                (static_cast<u32>(alpha * 255.0f) << 24) | 0x00FFFFFF;
        }
    }
    return device.createTexture(
        { .width = size, .height = size, .filter = rhi::FilterMode::Linear },
        pixels.data());
}

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    core::Log::init();
    LOG_INFO("True Adventurer - meadows engine, phase 0");

    auto window = platform::Window::create({ .title = "True Adventurer" });
    if (!window) {
        return 1;
    }

    auto device = rhi::Device::create(rhi::Backend::OpenGL, *window);
    if (!device) {
        return 1;
    }

    auto renderer = render::SpriteRenderer::create(*device);
    if (!renderer) {
        return 1;
    }

    const rhi::TextureHandle checker = createCheckerTexture(*device);
    const rhi::TextureHandle disc = createDiscTexture(*device);

    render::Camera2D camera { .position = { 0.0f, 0.0f }, .viewHeight = 12.0f };

    const auto startTime = std::chrono::steady_clock::now();
    while (window->pumpEvents()) {
        const f32 time = std::chrono::duration<f32>(
                             std::chrono::steady_clock::now() - startTime)
                             .count();
        const f32 aspect = static_cast<f32>(window->width()) /
                           static_cast<f32>(window->height());

        auto& cmd = device->beginFrame();
        cmd.beginRenderPass({ .loadOp = rhi::LoadOp::Clear,
                              .clearColor = { 0.10f, 0.12f, 0.16f, 1.0f } });

        renderer->begin(camera, aspect);

        // Meadow ground: one tinted checker tile per world unit.
        for (i32 y = -6; y < 6; ++y) {
            for (i32 x = -10; x < 10; ++x) {
                const f32 shade =
                    0.85f + 0.15f * static_cast<f32>((x * 7 + y * 13 + 60) % 5) / 4.0f;
                renderer->draw({
                    .position = { static_cast<f32>(x) + 0.5f,
                                  static_cast<f32>(y) + 0.5f },
                    .tint = { 0.35f * shade, 0.55f * shade, 0.30f * shade, 1.0f },
                    .texture = checker,
                });
            }
        }

        // Rotating ring of translucent discs over the ground.
        for (u32 k = 0; k < 8; ++k) {
            const f32 angle =
                time * 0.5f + static_cast<f32>(k) * glm::two_pi<f32>() / 8.0f;
            const f32 hue = static_cast<f32>(k) / 8.0f;
            renderer->draw({
                .position = { 4.0f * std::cos(angle), 4.0f * std::sin(angle) },
                .size = { 1.5f, 1.5f },
                .rotation = time,
                .tint = { 0.5f + 0.5f * std::cos(hue * glm::two_pi<f32>()),
                          0.5f + 0.5f * std::cos((hue + 0.33f) * glm::two_pi<f32>()),
                          0.5f + 0.5f * std::cos((hue + 0.67f) * glm::two_pi<f32>()),
                          0.85f },
                .texture = disc,
            });
        }

        renderer->end(cmd);
        cmd.endRenderPass();
        device->endFrame();
    }

    LOG_INFO("Shutting down");
    return 0;
}
