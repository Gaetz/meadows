#include "engine/core/Log.hpp"
#include "engine/platform/Window.hpp"

int main(int /*argc*/, char** /*argv*/) {
    core::Log::init();
    LOG_INFO("True Adventurer - meadows engine, phase 0");

    auto window = platform::Window::create({ .title = "True Adventurer" });
    if (!window) {
        return 1;
    }

    while (window->pumpEvents()) {
        // Frame work comes with the RHI milestone.
    }

    LOG_INFO("Shutting down");
    return 0;
}
