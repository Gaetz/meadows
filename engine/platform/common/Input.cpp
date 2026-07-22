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
    case Key::J:         return SDL_SCANCODE_J;
    case Key::R:         return SDL_SCANCODE_R;
    case Key::M:         return SDL_SCANCODE_M;
    // Left twin only: update() ORs the right one in, and the reverse map
    // below wants a single representative scancode per logical key.
    case Key::Alt:       return SDL_SCANCODE_LALT;
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

// Xbox naming -> SDL_Gamepad's positional layout. The
// triggers are handled as axes below (they have no SDL button).
SDL_GamepadButton sdlPadButtonFor(PadButton button) {
    switch (button) {
    case PadButton::A:               return SDL_GAMEPAD_BUTTON_SOUTH;
    case PadButton::B:               return SDL_GAMEPAD_BUTTON_EAST;
    case PadButton::X:               return SDL_GAMEPAD_BUTTON_WEST;
    case PadButton::Y:               return SDL_GAMEPAD_BUTTON_NORTH;
    case PadButton::LeftShoulder:    return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case PadButton::RightShoulder:   return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case PadButton::Start:           return SDL_GAMEPAD_BUTTON_START;
    case PadButton::Back:            return SDL_GAMEPAD_BUTTON_BACK;
    case PadButton::DPadUp:          return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case PadButton::DPadDown:        return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case PadButton::DPadLeft:        return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case PadButton::DPadRight:       return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    case PadButton::LeftStickClick:  return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case PadButton::RightStickClick: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case PadButton::LeftTrigger:
    case PadButton::RightTrigger:
    case PadButton::Count:           break;
    }
    return SDL_GAMEPAD_BUTTON_INVALID;
}

// The analog trigger reads as a logical button past this. [cpp-tuning]
constexpr f32 kTriggerButtonThreshold = 0.5f;

f32 normalizedAxis(SDL_Gamepad* pad, SDL_GamepadAxis axis) {
    return static_cast<f32>(SDL_GetGamepadAxis(pad, axis)) / 32767.0f;
}

} // namespace

Input::~Input() {
    if (gamepad) {
        SDL_CloseGamepad(static_cast<SDL_Gamepad*>(gamepad));
    }
}

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
    // Hotplug — the FIRST pad that appears becomes the
    // active one (SDL3 also fires ADDED for pads present at init);
    // its removal frees the slot for the next.
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!gamepad) {
            gamepad = SDL_OpenGamepad(event->gdevice.which);
            if (gamepad) {
                gamepadId = event->gdevice.which;
            }
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (gamepad && event->gdevice.which == gamepadId) {
            SDL_CloseGamepad(static_cast<SDL_Gamepad*>(gamepad));
            gamepad = nullptr;
            gamepadId = 0;
        }
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
        // Alt/Option is a modifier: no side preference, either twin counts.
        current[static_cast<size_t>(Key::Alt)] =
            state[SDL_SCANCODE_LALT] || state[SDL_SCANCODE_RALT];
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
    // Gamepad snapshot — polled like the keyboard; all
    // dead when no pad is connected.
    padPrevious = padCurrent;
    if (auto* pad = static_cast<SDL_Gamepad*>(gamepad)) {
        leftTriggerValue =
            normalizedAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        rightTriggerValue =
            normalizedAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        // SDL's stick Y is positive DOWN; gameplay reads y = up/forward.
        leftStickValue = applyDeadzone(
            { normalizedAxis(pad, SDL_GAMEPAD_AXIS_LEFTX),
              -normalizedAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) },
            stickDeadzone);
        rightStickValue = applyDeadzone(
            { normalizedAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX),
              -normalizedAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) },
            stickDeadzone);
        for (size_t i = 0; i < padCurrent.size(); ++i) {
            const auto button = static_cast<PadButton>(i);
            if (button == PadButton::LeftTrigger) {
                padCurrent[i] = leftTriggerValue >= kTriggerButtonThreshold;
            } else if (button == PadButton::RightTrigger) {
                padCurrent[i] = rightTriggerValue >= kTriggerButtonThreshold;
            } else {
                padCurrent[i] =
                    SDL_GetGamepadButton(pad, sdlPadButtonFor(button));
            }
        }
    } else {
        padCurrent = {};
        leftStickValue = {};
        rightStickValue = {};
        leftTriggerValue = 0.0f;
        rightTriggerValue = 0.0f;
    }
}

bool Input::isDown(Key key) const {
    return current[static_cast<size_t>(key)];
}

bool Input::padConnected() const {
    return gamepad != nullptr;
}

bool Input::padDown(PadButton button) const {
    return padCurrent[static_cast<size_t>(button)];
}

bool Input::padPressed(PadButton button) const {
    const auto index = static_cast<size_t>(button);
    return padCurrent[index] && !padPrevious[index];
}

Vec2 Input::leftStick() const {
    return leftStickValue;
}

Vec2 Input::rightStick() const {
    return rightStickValue;
}

f32 Input::leftTrigger() const {
    return leftTriggerValue;
}

f32 Input::rightTrigger() const {
    return rightTriggerValue;
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

bool Input::mouseReleased(MouseButton button) const {
    const auto index = static_cast<size_t>(button);
    return !mouseCurrent[index] && mousePrevious[index];
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
