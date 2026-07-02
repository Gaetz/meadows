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
    case Key::Shift:  return SDL_SCANCODE_LSHIFT;
    case Key::Count:  break;
    }
    return SDL_SCANCODE_UNKNOWN;
}

int sdlButtonFor(MouseButton button) {
    switch (button) {
    case MouseButton::Left:   return SDL_BUTTON_LEFT;
    case MouseButton::Right:  return SDL_BUTTON_RIGHT;
    case MouseButton::Middle: return SDL_BUTTON_MIDDLE;
    case MouseButton::Count:  break;
    }
    return 0;
}

} // namespace

void Input::update() {
    previous = current;
    mousePrevious = mouseCurrent;
    // Valid after SDL_PumpEvents, which the window's event pump runs each frame.
    const bool* state = SDL_GetKeyboardState(nullptr);
    if (state) {
        for (size_t i = 0; i < current.size(); ++i) {
            current[i] = state[scancodeFor(static_cast<Key>(i))];
        }
    }
    // SDL3: position is filled as floats; the return value is the button bitmask.
    float mx = 0.0f, my = 0.0f;
    const SDL_MouseButtonFlags mask = SDL_GetMouseState(&mx, &my);
    mousePos = { mx, my };
    for (size_t i = 0; i < mouseCurrent.size(); ++i) {
        mouseCurrent[i] =
            (mask & SDL_BUTTON_MASK(sdlButtonFor(static_cast<MouseButton>(i)))) != 0;
    }
}

bool Input::isDown(Key key) const {
    return current[static_cast<size_t>(key)];
}

bool Input::wasPressed(Key key) const {
    const auto index = static_cast<size_t>(key);
    return current[index] && !previous[index];
}

Vec2 Input::mousePosition() const {
    return mousePos;
}

bool Input::mouseDown(MouseButton button) const {
    return mouseCurrent[static_cast<size_t>(button)];
}

bool Input::mousePressed(MouseButton button) const {
    const auto index = static_cast<size_t>(button);
    return mouseCurrent[index] && !mousePrevious[index];
}

} // namespace platform
