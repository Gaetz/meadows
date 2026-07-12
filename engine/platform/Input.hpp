#pragma once

#include <array>
#include <cmath>

#include <glm/vec2.hpp> // full Vec2 type (Defines.hpp only forward-declares glm)

#include "engine/core/Defines.hpp"

namespace platform {

// Logical keys the game queries. Mapped to SDL scancodes in the .cpp, so this
// header stays platform-clean (§3.1). Extend as gameplay needs more.
enum class Key : u16 {
    W, A, S, D,
    Up, Down, Left, Right,
    Space, Enter, Escape,
    E, F, Q, Shift, Ctrl,
    Num1, Num2, Num3, Num4, Num5,
    // UI keys (chantier 4): text editing + game-screen hotkeys.
    Tab, Backspace, Delete, Home, End, PageUp, PageDown,
    I, T,
    J, // quest journal (chantier 6)
    R, // draw/sheathe the weapon (P0 combat)
    Count
};

// Logical mouse buttons. Mapped to SDL button constants in the .cpp.
enum class MouseButton : u8 {
    Left, Right, Middle,
    Count
};

// Logical gamepad buttons (chantier 9) — Xbox naming over SDL_Gamepad's
// positional layout, mapped in the .cpp (§3.1: the header stays
// platform-clean). The triggers are analog axes; past a threshold they
// ALSO read as logical buttons, so bindings can treat them like any
// other button.
enum class PadButton : u8 {
    A, B, X, Y,
    LeftShoulder, RightShoulder,
    LeftTrigger, RightTrigger, // analog past the threshold = "down"
    Start, Back,
    DPadUp, DPadDown, DPadLeft, DPadRight,
    LeftStickClick, RightStickClick,
    Count
};

// Polled keyboard + mouse state. Engine::loop calls update() once per frame after
// the event pump; the game queries it via Engine::getInput(). Held-vs-pressed is
// derived from this frame's and last frame's snapshots. Mouse position is in
// screen pixels (top-left origin); project to world via render::screenToWorld.
class Input {
public:
    ~Input(); // closes the open gamepad, if any

    void update(); // snapshot keyboard + mouse + gamepad state

    bool isDown(Key key) const;     // held this frame
    bool wasPressed(Key key) const; // edge: down this frame, up last frame

    // --- Gamepad (chantier 9) — ONE active pad, hotplugged ------------------
    // handleEvent opens the first pad that appears and follows removal;
    // update() polls it like the keyboard. All dead when no pad is on.
    bool padConnected() const;
    bool padDown(PadButton button) const;     // held this frame
    bool padPressed(PadButton button) const;  // edge: down this, up last
    // Sticks with the RADIAL deadzone applied, rescaled so full deflection
    // stays reachable. x = right; y = UP/forward (SDL's down-positive Y is
    // negated here so gameplay math reads naturally).
    Vec2 leftStick() const;
    Vec2 rightStick() const;
    f32 leftTrigger() const;  // 0..1, raw analog
    f32 rightTrigger() const;
    // The settings screen feeds this (machine preference, not a Form).
    void setStickDeadzone(f32 zone) { stickDeadzone = zone; }
    f32 stickDeadzoneValue() const { return stickDeadzone; }

    Vec2 mousePosition() const;             // screen pixels, top-left origin
    // Mouse motion since last frame, in pixels. Keeps accumulating in relative
    // mouse mode (Window::setRelativeMouseMode), where mousePosition freezes.
    Vec2 mouseDelta() const;
    bool mouseDown(MouseButton button) const;     // held this frame
    bool mousePressed(MouseButton button) const;  // edge: down this, up last
    bool mouseReleased(MouseButton button) const; // edge: up this, down last

    // --- Event-fed channel (chantier 4, game UI) -----------------------------
    // Engine's event hook feeds raw platform events here during pumpEvents;
    // update() then publishes what accumulated as this frame's data. Unlike
    // the polled snapshot above, this preserves ordering and OS key repeat —
    // what text fields need.
    void handleEvent(const void* nativeEvent); // opaque SDL_Event (§3.1)

    struct KeyEvent {
        Key key;
        bool down;
    };
    const vector<KeyEvent>& keyEvents() const;  // this frame, repeats included
    const str& textInput() const;               // UTF-8 typed this frame
    f32 wheelDelta() const;                     // vertical scroll this frame

private:
    array<bool, static_cast<size_t>(Key::Count)> current {};
    array<bool, static_cast<size_t>(Key::Count)> previous {};
    array<bool, static_cast<size_t>(MouseButton::Count)> mouseCurrent {};
    array<bool, static_cast<size_t>(MouseButton::Count)> mousePrevious {};
    Vec2 mousePos {};      // screen pixels
    Vec2 mouseDeltaPx {};  // pixels moved since last update()

    // Gamepad state (chantier 9). The handle is opaque (§3.1: no SDL type
    // in this header); id tracks which device we own for hotplug removal.
    array<bool, static_cast<size_t>(PadButton::Count)> padCurrent {};
    array<bool, static_cast<size_t>(PadButton::Count)> padPrevious {};
    Vec2 leftStickValue {};
    Vec2 rightStickValue {};
    f32 leftTriggerValue { 0.0f };
    f32 rightTriggerValue { 0.0f };
    f32 stickDeadzone { 0.15f };
    void* gamepad { nullptr }; // SDL_Gamepad*
    u32 gamepadId { 0 };       // SDL_JoystickID

    // Accumulated during pumpEvents, published by update().
    vector<KeyEvent> pendingKeyEvents;
    str pendingText;
    f32 pendingWheel { 0.0f };
    vector<KeyEvent> frameKeyEvents;
    str frameText;
    f32 frameWheel { 0.0f };
};

// The RADIAL stick deadzone (chantier 9): kills drift below `zone`, then
// rescales the live band to 0..1 so full deflection stays reachable and
// the response has no step at the zone edge. Pure — doctested (tests
// have no SDL; this is the testable half of the gamepad channel).
inline Vec2 applyDeadzone(Vec2 stick, f32 zone) {
    const f32 len = std::sqrt(stick.x * stick.x + stick.y * stick.y);
    if (len <= zone || zone >= 1.0f) {
        return Vec2 { 0.0f, 0.0f };
    }
    f32 scaled = (len - zone) / (1.0f - zone);
    if (scaled > 1.0f) {
        scaled = 1.0f; // corners can exceed unit length on square gates
    }
    return stick * (scaled / len);
}

// WASD (+ optionally the arrow keys) -> the raw ±1 movement axis
// (x = right, y = forward). Callers normalize and compose with their own
// basis — screen axes in 2D, the camera basis in first person (audit
// U5-8: this pair of ifs was rewritten per scene). The LEFT STICK feeds
// the same axis when the keyboard is silent (keyboard wins — no drift
// fight, and the deadzone already zeroed a resting stick).
inline Vec2 moveAxis(const Input& input, bool includeArrows = false) {
    Vec2 axis { 0.0f, 0.0f };
    if (input.isDown(Key::W) || (includeArrows && input.isDown(Key::Up))) {
        axis.y += 1.0f;
    }
    if (input.isDown(Key::S) || (includeArrows && input.isDown(Key::Down))) {
        axis.y -= 1.0f;
    }
    if (input.isDown(Key::D) || (includeArrows && input.isDown(Key::Right))) {
        axis.x += 1.0f;
    }
    if (input.isDown(Key::A) || (includeArrows && input.isDown(Key::Left))) {
        axis.x -= 1.0f;
    }
    if (axis.x == 0.0f && axis.y == 0.0f) {
        axis = input.leftStick();
    }
    return axis;
}

} // namespace platform
