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
    case Key::Q:      return SDL_SCANCODE_Q;
    case Key::Shift:  return SDL_SCANCODE_LSHIFT;
    case Key::Ctrl:   return SDL_SCANCODE_LCTRL;
    case Key::Num1:   return SDL_SCANCODE_1;
    case Key::Num2:   return SDL_SCANCODE_2;
    case Key::Num3:   return SDL_SCANCODE_3;
    case Key::Num4:   return SDL_SCANCODE_4;
    case Key::Num5:   return SDL_SCANCODE_5;
    case Key::Tab:       return SDL_SCANCODE_TAB;
    case Key::Backspace: return SDL_SCANCODE_BACKSPACE;
    case Key::Delete:    return SDL_SCANCODE_DELETE;
    case Key::Home:      return SDL_SCANCODE_HOME;
    case Key::End:       return SDL_SCANCODE_END;
    case Key::PageUp:    return SDL_SCANCODE_PAGEUP;
    case Key::PageDown:  return SDL_SCANCODE_PAGEDOWN;
    case Key::I:         return SDL_SCANCODE_I;
    case Key::T:         return SDL_SCANCODE_T;
    case Key::Count:  break;
    }
    return SDL_SCANCODE_UNKNOWN;
}

// Reverse map for the event-fed channel: only the keys the UI cares about.
Key keyForScancode(SDL_Scancode scancode) {
    for (u16 i = 0; i < static_cast<u16>(Key::Count); ++i) {
        if (scancodeFor(static_cast<Key>(i)) == scancode) {
            return static_cast<Key>(i);
        }
    }
    return Key::Count;
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

void Input::handleEvent(const void* nativeEvent) {
    const auto* event = static_cast<const SDL_Event*>(nativeEvent);
    switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const Key key = keyForScancode(event->key.scancode);
        if (key != Key::Count) {
            pendingKeyEvents.push_back(
                { key, event->type == SDL_EVENT_KEY_DOWN });
        }
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
        pendingText += event->text.text;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        pendingWheel += event->wheel.y;
        break;
    default:
        break;
    }
}

void Input::update() {
    previous = current;
    mousePrevious = mouseCurrent;
    // Publish what pumpEvents accumulated since the last frame.
    frameKeyEvents = std::move(pendingKeyEvents);
    pendingKeyEvents.clear();
    frameText = std::move(pendingText);
    pendingText.clear();
    frameWheel = pendingWheel;
    pendingWheel = 0.0f;
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
    // Accumulated motion since the previous call — works in relative mouse
    // mode too, where the absolute position stops moving.
    float dx = 0.0f, dy = 0.0f;
    SDL_GetRelativeMouseState(&dx, &dy);
    mouseDeltaPx = { dx, dy };
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

Vec2 Input::mouseDelta() const {
    return mouseDeltaPx;
}

bool Input::mouseDown(MouseButton button) const {
    return mouseCurrent[static_cast<size_t>(button)];
}

bool Input::mousePressed(MouseButton button) const {
    const auto index = static_cast<size_t>(button);
    return mouseCurrent[index] && !mousePrevious[index];
}

const vector<Input::KeyEvent>& Input::keyEvents() const {
    return frameKeyEvents;
}

const str& Input::textInput() const {
    return frameText;
}

f32 Input::wheelDelta() const {
    return frameWheel;
}

} // namespace platform
