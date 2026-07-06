#pragma once

#include "engine/core/Defines.hpp"

namespace platform {
class Window;
}

namespace ui {

// Dear ImGui integration for the dev UI, using the stock SDL3 + OpenGL3
// backends. This is the one spot outside platform/ and the GL backend that
// knowingly touches SDL and GL: the stock imgui_impl files are inherently
// coupled to them, and rewriting them on top of the RHI would be
// gold-plating (§1). A future Vulkan RHI backend brings imgui_impl_vulkan,
// selected at runtime alongside it.
class ImGuiLayer {
public:
    // Requires the window's GL context to be current (i.e. create the RHI
    // device first). Returns nullptr (with a logged error) on failure.
    static uptr<ImGuiLayer> create(platform::Window& window);
    ~ImGuiLayer();
    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Starts an ImGui frame; emit widgets between beginFrame and render.
    void beginFrame();
    // Renders the accumulated UI straight into the current backbuffer,
    // after the scene's render pass.
    void render();

    // Feeds one raw platform event to the ImGui backend. The Engine owns
    // the window event hook (it fans out to ImGui AND platform::Input) and
    // calls this from it.
    static void processEvent(const void* nativeEvent);

private:
    explicit ImGuiLayer(platform::Window& window);

    platform::Window& window;
};

} // namespace ui
