#pragma once

#include <array>

#include "engine/core/Defines.hpp"

namespace platform {

// Logical keys the game queries. Mapped to SDL scancodes in the .cpp, so this
// header stays platform-clean (§3.1). Extend as gameplay needs more.
enum class Key : u16 {
    W, A, S, D,
    Up, Down, Left, Right,
    Space, Enter, Escape,
    E, F,
    Count
};

// Polled keyboard state. Engine::loop calls update() once per frame after the
// event pump; the game queries it via Engine::getInput(). Held-vs-pressed is
// derived from this frame's and last frame's snapshots.
class Input {
public:
    void update(); // snapshot the keyboard state (current -> previous)

    bool isDown(Key key) const;     // held this frame
    bool wasPressed(Key key) const; // edge: down this frame, up last frame

private:
    array<bool, static_cast<size_t>(Key::Count)> current {};
    array<bool, static_cast<size_t>(Key::Count)> previous {};
};

} // namespace platform
