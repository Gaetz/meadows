#include "engine/platform/Input.hpp"

#include <SDL3/SDL.h>

namespace platform {

namespace {

SDL_Scancode scancodeFor(Key key) {
    switch (key) {
    case Key::W:      return SDL_SCANCODE_W;
    case Key::A:      return SDL_SCANCODE_A;
    case Key::S:      return SDL_SCANCODE_S;
    case Key::D:      return SDL_SCANCODE_D;
    case Key::Up:     return SDL_SCANCODE_UP;
    case Key::Down:   return SDL_SCANCODE_DOWN;
    case Key::Left:   return SDL_SCANCODE_LEFT;
    case Key::Right:  return SDL_SCANCODE_RIGHT;
    case Key::Space:  return SDL_SCANCODE_SPACE;
    case Key::Enter:  return SDL_SCANCODE_RETURN;
    case Key::Escape: return SDL_SCANCODE_ESCAPE;
    case Key::E:      return SDL_SCANCODE_E;
    case Key::F:      return SDL_SCANCODE_F;
    case Key::Count:  break;
    }
    return SDL_SCANCODE_UNKNOWN;
}

} // namespace

void Input::update() {
    previous = current;
    // Valid after SDL_PumpEvents, which the window's event pump runs each frame.
    const bool* state = SDL_GetKeyboardState(nullptr);
    if (!state) {
        return;
    }
    for (size_t i = 0; i < current.size(); ++i) {
        current[i] = state[scancodeFor(static_cast<Key>(i))];
    }
}

bool Input::isDown(Key key) const {
    return current[static_cast<size_t>(key)];
}

bool Input::wasPressed(Key key) const {
    const auto index = static_cast<size_t>(key);
    return current[index] && !previous[index];
}

} // namespace platform
