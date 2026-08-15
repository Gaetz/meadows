#pragma once

#include "engine/core/Defines.hpp"

namespace game {

// The game-screen stack — PURE logic, headless-testable.
// Screens are defined from UiScreenForm records by the scene (this class
// stays data-free so meadows-runtime needs no meadows-data dep):
//  - overlay screens (HUD) are always-on layers under everything;
//  - modal screens stack; the topmost gets input, Escape pops it.
// The scene diffs visibleScreens() each frame against what UiSystem shows
// (showDocument/closeDocument) — this class never touches RmlUi.
class ScreenStack {
public:
    struct Screen {
        str name;     // logical name ("hud", "inventory"...)
        str document; // .rml path under the plugin ui/ roots
        bool modal { false };
        bool overlay { false };
    };

    // (Re)defines a screen; later definitions win (mod layering order).
    void define(Screen screen);
    const Screen* find(const str& name) const;

    // Overlay -> visible; modal -> pushed (no duplicates: re-show raises
    // it to the top). Returns false for unknown names.
    bool show(const str& name);
    // Hides an overlay or removes a modal wherever it sits in the stack.
    bool close(const str& name);
    // Pops the top modal; false when no modal is open (Escape falls
    // through to the scene: pause menu, etc.).
    bool closeTop();
    void closeAll(); // modals only — overlays are the scene's HUD state

    bool modalOpen() const { return !stack.empty(); }
    const Screen* topModal() const;

    // Draw order: overlays first (bottom), then the modal stack.
    vector<const Screen*> visibleScreens() const;

private:
    vector<Screen> screens;
    vector<str> stack;    // modal names, bottom -> top
    vector<str> overlays; // visible overlay names, in SHOW order (first
                          // shown = bottom-most layer, stack semantics)
};

} // namespace game
