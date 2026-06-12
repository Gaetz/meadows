#include "engine/core/Log.hpp"
#include "engine/platform/Window.hpp"
#include "engine/rhi/Device.hpp"

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

    while (window->pumpEvents()) {
        auto& cmd = device->beginFrame();
        cmd.beginRenderPass({ .loadOp = rhi::LoadOp::Clear,
                              .clearColor = { 0.39f, 0.58f, 0.93f, 1.0f } });
        cmd.endRenderPass();
        device->endFrame();
    }

    LOG_INFO("Shutting down");
    return 0;
}
