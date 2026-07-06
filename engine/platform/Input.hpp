#pragma once

#include <array>

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
    Count
};

// Logical mouse buttons. Mapped to SDL button constants in the .cpp.
enum class MouseButton : u8 {
    Left, Right, Middle,
    Count
};

// Polled keyboard + mouse state. Engine::loop calls update() once per frame after
// the event pump; the game queries it via Engine::getInput(). Held-vs-pressed is
// derived from this frame's and last frame's snapshots. Mouse position is in
// screen pixels (top-left origin); project to world via render::screenToWorld.
class Input {
public:
    void update(); // snapshot keyboard + mouse state (current -> previous)

    bool isDown(Key key) const;     // held this frame
    bool wasPressed(Key key) const; // edge: down this frame, up last frame

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

    // Accumulated during pumpEvents, published by update().
    vector<KeyEvent> pendingKeyEvents;
    str pendingText;
    f32 pendingWheel { 0.0f };
    vector<KeyEvent> frameKeyEvents;
    str frameText;
    f32 frameWheel { 0.0f };
};

} // namespace platform
