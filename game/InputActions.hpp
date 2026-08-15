#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "engine/core/Defines.hpp"
#include "engine/platform/Input.hpp"

// The ACTION layer. Gameplay reads INTENTIONS; the
// ActionMap owns which physical inputs mean what — one keyboard key,
// one mouse button AND one pad button per action, remappable, persisted
// in settings.toml (a machine preference, deliberately NOT a Form).
// Move/Look are analog AXES and stay platform::moveAxis / mouseDelta +
// rightStick — only button-like intents live here.

namespace game {

enum class InputAction : u8 {
    Attack,      // LMB / RT — melee swing, bow draw+release
    Block,       // RMB / LT — the raised guard
    DrawSheathe, // R / Y
    Sneak,       // Ctrl / left-stick click (toggle)
    SprintDodge, // Shift / B — hold sprints, tap dodges
    Jump,        // Space / A (swim-up too)
    Interact,    // E / X
    Inventory,   // I / d-pad up
    Journal,     // J / Back
    WaitMenu,    // T / d-pad down
    Map,         // M / d-pad left
    Pause,       // Escape / Start
    // The ALTERNATE interaction on the prompted entity — a follower's
    // gear screen today, the grave's container later. Bindings are single
    // inputs (one key, one pad button), so the design's Shift+E chord
    // becomes F / LB; a chord-modifier extension of Binding stays
    // possible later without touching callers.
    InteractAlt, // F / LB
    Count
};

// One physical chord per device class; Count = that slot is unbound.
struct Binding {
    platform::Key key { platform::Key::Count };
    platform::MouseButton mouse { platform::MouseButton::Count };
    platform::PadButton pad { platform::PadButton::Count };
};

class ActionMap {
public:
    ActionMap(); // the default layout (today's keyboard + a fixed pad)

    // An action fires from ANY of its bound inputs.
    bool down(const platform::Input& input, InputAction action) const;
    bool pressed(const platform::Input& input, InputAction action) const;

    const Binding& binding(InputAction action) const;

    // THE mutation points (one owner per state change): a
    // rebind STEALS the physical input from whatever action held it —
    // the old owner's slot goes unbound, no silent double-binding.
    void rebindKey(InputAction action, platform::Key key);
    void rebindMouse(InputAction action, platform::MouseButton button);
    void rebindPad(InputAction action, platform::PadButton button);

private:
    std::array<Binding, static_cast<size_t>(InputAction::Count)>
        bindings {};
};

// Stable names for settings.toml and the options screen. parse* returns
// nullopt for unknown names (a stale settings file falls back to the
// default binding instead of crashing).
std::string_view actionName(InputAction action);
std::optional<InputAction> parseAction(std::string_view name);
std::string_view keyName(platform::Key key);
std::optional<platform::Key> parseKey(std::string_view name);
std::string_view mouseButtonName(platform::MouseButton button);
std::optional<platform::MouseButton> parseMouseButton(std::string_view name);
std::string_view padButtonName(platform::PadButton button);
std::optional<platform::PadButton> parsePadButton(std::string_view name);

} // namespace game

namespace render {
class FlyCamera;
}

namespace game {

struct Settings;

// The shared first-person mouselook (on foot and mounted): mouse pixels
// x base sensitivity x user multiplier, right stick rad/s x dt, ONE
// invert-Y switch covering both, pitch clamped to +/-89 degrees.
void applyLookInput(render::FlyCamera& flyCamera, platform::Input& input,
                    const Settings* settings, f32 dt);

} // namespace game
