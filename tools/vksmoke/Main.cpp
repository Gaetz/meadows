// vksmoke — Vulkan bring-up harness (chantier VULKAN, brique V1).
//
// Drives the Vulkan backend end to end (instance -> surface -> swapchain ->
// submit -> present) WITHOUT the Engine loop, which still depends on the GL
// SpriteRenderer and the GL ImGui layer (ported in V3/V6). It clears the
// backbuffer with an animated color: a window that fades through the ramp is
// the visual proof the whole presentation path works — on macOS, through
// MoltenVK.
//
// Run: ./vksmoke [seconds]   (default 5; 0 = until the window is closed)

#include <cmath>
#include <cstdlib>

#include "engine/core/Clock.hpp"
#include "engine/core/Defines.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Window.hpp"
#include "engine/rhi/Device.hpp"

int main(int argc, char** argv) {
    core::Log::init();

    f64 seconds = 5.0;
    if (argc > 1) {
        seconds = std::atof(argv[1]);
    }

    // The surface flag is fixed at window creation and must match the backend.
    auto window = platform::Window::create(
        { .title = "Meadows — Vulkan smoke (V1)",
          .width = 1280,
          .height = 720,
          .api = platform::GraphicsApi::Vulkan });
    if (!window) {
        LOG_ERROR("vksmoke: window creation failed");
        return 1;
    }

    auto device = rhi::Device::create(rhi::Backend::Vulkan, *window);
    if (!device) {
        LOG_ERROR("vksmoke: Vulkan device creation failed");
        return 1;
    }

    const auto start = core::clockNow();
    f64 elapsed = 0.0;
    u32 frames = 0;
    while (window->pumpEvents()) {
        elapsed = core::secondsBetween(start, core::clockNow());
        if (seconds > 0.0 && elapsed >= seconds) {
            break;
        }
        // Animated so a static frame (or a frozen swapchain) is obvious.
        const f32 t = static_cast<f32>(elapsed);
        const f32 pulse = 0.5f + 0.5f * std::sin(t * 2.0f);

        auto& cmd = device->beginFrame();
        cmd.beginRenderPass({ .clearColor = { 0.10f + 0.35f * pulse,
                                              0.12f + 0.20f * pulse,
                                              0.30f + 0.45f * pulse, 1.0f } });
        cmd.endRenderPass();
        device->endFrame();
        ++frames;
    }

    LOG_INFO("vksmoke: {} frames in {:.2f}s ({:.1f} fps) — presentation path OK",
             frames, elapsed, elapsed > 0.0 ? frames / elapsed : 0.0);
    return 0;
}
