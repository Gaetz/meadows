#pragma once

#include <filesystem>

#include "engine/core/Defines.hpp"
#include "engine/rhi/Rhi.hpp"

namespace rhi {
class CommandBuffer;
class Device;
}
namespace render {
class ShaderLibrary;
}

namespace ui {

// The game-UI seam (horizontal pass H4): RmlUi behind a narrow facade —
// no Rml type crosses this header. Documents (.rml/.rcss) resolve through
// an ordered list of root directories, LAST ROOT WINS: feed it every
// plugin's ui/ dir in load order and a mod overrides a screen by shipping
// the same path (decision 2026-07-05 — the SkyUI model).
//
// Rendering goes through rhi:: only (compiled geometry = static
// vertex/index buffers; scissor + premultiplied alpha were added to the
// RHI for this). render() records into the CURRENT render pass — call it
// inside the backbuffer pass, after the world, before ImGui.
//
// HOW TO FILL (post-7/07, "interfaces" vertical):
//  - screens: UiScreenForm registry -> showScreen(name) (modal stack,
//    overlay HUD), documents from the plugin roots;
//  - input: route mouse/keyboard/gamepad from platform::Input into
//    processMouse*/processKey/processText below;
//  - data binding: Rml data models bound to game state (health bars,
//    inventory grids) — add a DataModel facade here, keep Rml types out;
//  - localization: resolve loc keys in documents via LocStringForm.
class UiSystem {
public:
    UiSystem();
    ~UiSystem();
    UiSystem(const UiSystem&) = delete;
    UiSystem& operator=(const UiSystem&) = delete;

    bool create(rhi::Device& device, render::ShaderLibrary& shaders,
                vector<std::filesystem::path> documentRoots, u32 width,
                u32 height);
    void destroy(rhi::Device& device);

    // Fonts must load before the first document (RmlUi requirement).
    bool loadFont(const std::filesystem::path& path);

    // Loads + shows a document by root-relative path ("hello.rml").
    bool showDocument(const str& path);
    void closeDocuments();

    void resize(u32 width, u32 height);
    void update(f32 dt);
    void render(rhi::CommandBuffer& cmd, rhi::Device& device, u32 width,
                u32 height);

    // Input routing (viewport pixel coordinates).
    void processMouseMove(i32 x, i32 y);
    void processMouseButton(i32 button, bool down);
    void processMouseWheel(f32 delta);

    bool ready() const { return created; }

    struct Impl;

private:
    uptr<Impl> pimpl;
    bool created { false };
};

} // namespace ui
