#pragma once

#include <filesystem>
#include <functional>

#include "engine/core/Defines.hpp"
#include "engine/platform/Input.hpp"
#include "engine/rhi/Rhi.hpp"

namespace rhi {
class CommandBuffer;
class Device;
}
namespace render {
class ShaderLibrary;
}

namespace ui {

// One row of a data-model table (inventory, barter, dialogue choices...).
// Fixed named cells keep the Rml binding flat and the .rml free to lay out
// whichever columns it wants ({{ row.c0 }} ... {{ row.c4 }}). `id` comes
// back in row events; `tag` is a freeform CSS hook ("equipped", "hostile").
struct UiRow {
    str id;
    str c0, c1, c2, c3, c4;
    bool selected { false };
    str tag;

    bool operator==(const UiRow&) const = default;
};

// Declares a data model and its slots up front — RmlUi freezes a model's
// bindings at creation, so every name a document references must be listed
// here, BEFORE that document loads.
struct UiModelDesc {
    str name;
    vector<str> numbers; // bound as f64
    vector<str> strings;
    vector<str> bools;
    bool rows { false };          // binds a "rows" array of UiRow
    vector<str> events;           // data-event callbacks by name
};

// Callback for data-event bindings: model name, event name, stringified
// arguments (e.g. <div data-event-click="pick(row.id)">).
using UiModelEventHandler = std::function<void(
    const str& model, const str& event, const vector<str>& args)>;

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

    // Loads + shows a document by root-relative path ("hud.rml"). Loaded
    // documents are kept by path: showing an already-loaded document just
    // makes it visible again (screen stack friendly).
    bool showDocument(const str& path);
    // Hides + unloads one document by the path it was shown with.
    void closeDocument(const str& path);
    void closeDocuments();

    void resize(u32 width, u32 height);
    void update(f32 dt);
    void render(rhi::CommandBuffer& cmd, rhi::Device& device, u32 width,
                u32 height);

    // Input routing (viewport pixel coordinates).
    void processMouseMove(i32 x, i32 y);
    void processMouseButton(i32 button, bool down);
    void processMouseWheel(f32 delta);
    // Key events (edges + OS repeat) and UTF-8 text — feed from
    // platform::Input::keyEvents()/textInput() while a screen is open.
    void processKey(platform::Key key, bool down);
    void processTextInput(const str& utf8);
    // True while an Rml text field has the focus (route text there, and
    // enable Window::setTextInput while it holds).
    bool textFieldFocused() const;

    // --- Gamepad / keyboard focus navigation (C9.3) --------------------------
    // RmlUi 6.1 moves the focus spatially on arrow keydowns when the
    // focused element carries the `nav`/`nav-*` RCSS properties and the
    // targets are `tab-index: auto`; Enter/Space click the focused
    // element. These three close the loop for a pad: land the focus,
    // detect it got lost, activate.
    //
    // Focuses the first focusable element of a shown document (by the
    // path it was shown with), preferring one carrying the "selected"
    // class so a rebuilt item list puts the pad back on the picked row.
    // Call after update() — a freshly shown document only has computed
    // styles (and its data-for rows) once the context updated.
    bool focusFirst(const str& documentPath);
    // True while a visible, navigable (tab-index: auto) element holds
    // the focus — when false the pad has nowhere to move from and the
    // caller should focusFirst() the top screen.
    bool hasNavigableFocus() const;
    // Clicks the focused element (pad A): dispatches the same click a
    // mouse press would, exactly like RmlUi's built-in Enter handling.
    // False when nothing navigable holds the focus.
    bool activateFocused();

    // --- Data models (B2) ---------------------------------------------------
    // Create before the documents that reference them load; set* pushes a
    // value and dirties the binding. All Rml types stay in the .cpp.
    bool createModel(const UiModelDesc& desc);
    void setModelEventHandler(UiModelEventHandler handler);
    void setNumber(const str& model, const str& slot, f64 value);
    void setString(const str& model, const str& slot, const str& value);
    void setBool(const str& model, const str& slot, bool value);
    void setRows(const str& model, vector<UiRow> rows);
    // Reads a string slot back — two-way bindings (data-value on a text
    // field) write into it (search boxes, name inputs).
    str getString(const str& model, const str& slot) const;

    bool ready() const { return created; }

    struct Impl;

private:
    uptr<Impl> pimpl;
    bool created { false };
};

} // namespace ui
