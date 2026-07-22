#pragma once

#include "engine/core/Defines.hpp"

namespace data {
class FormDatabase;
class FormTypeRegistry;
class EditSession;
}
namespace script {
class Vm;
}

namespace game {

class ConsolePanel;

// The F8 dev console's infrastructure extracted from LandscapeScene:
// the reflection edit-session, the shared Lua VM, the ConsolePanel
// and the visibility toggle. The WORLD commands (spawn/tp/tgm/save/settime…)
// are registered by the scene onto panel() — they touch scene internals
// (spawn into the world, teleport the player), so they stay where `this` is
// stable, the same rationale that keeps the event-bus subscriptions in the
// scene. God mode is a console-only cheat, so it lives here and combat reads
// it through godMode().
class SceneConsole {
public:
    SceneConsole();
    ~SceneConsole();

    // onEnter: build the session / VM / panel over the freshly-resolved
    // forms. Returns the panel so the scene registers its world commands.
    ConsolePanel& create(data::FormDatabase& forms,
                         data::FormTypeRegistry& formTypes);
    // onExit: drop everything (references forms/session — before re-resolve).
    void reset();

    // F8: flip visibility. Closing during Play releases the ImGui nav-focus
    // latch (the enterPlayMode fix) so Escape / clicks reach the game again.
    void toggle(bool playMode);
    void draw() const;
    bool visible() const { return visible_; }

    // The scene-shared Lua VM (null before create()/after reset()) — the
    // trigger volumes run their TriggerForm.script snippets through it.
    script::Vm* vm() { return vm_.get(); }

    bool godMode() const { return godMode_; }
    bool toggleGodMode() {
        godMode_ = !godMode_;
        return godMode_;
    }

private:
    uptr<data::EditSession> session_;
    uptr<script::Vm> vm_;
    uptr<ConsolePanel> console_;
    bool visible_ { false };
    bool godMode_ { false };
};

} // namespace game
