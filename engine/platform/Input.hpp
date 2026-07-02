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
    E, F, Shift,
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
    bool mouseDown(MouseButton button) const;    // held this frame
    bool mousePressed(MouseButton button) const; // edge: down this, up last

private:
    array<bool, static_cast<size_t>(Key::Count)> current {};
    array<bool, static_cast<size_t>(Key::Count)> previous {};
    array<bool, static_cast<size_t>(MouseButton::Count)> mouseCurrent {};
    array<bool, static_cast<size_t>(MouseButton::Count)> mousePrevious {};
    Vec2 mousePos {}; // screen pixels
};

} // namespace platform
