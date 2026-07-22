#include "game/InputActions.hpp"

namespace game {

using platform::Key;
using platform::MouseButton;
using platform::PadButton;

ActionMap::ActionMap() {
    const auto set = [this](InputAction action, Key key, MouseButton mouse,
                            PadButton pad) {
        bindings[static_cast<size_t>(action)] = { key, mouse, pad };
    };
    // The default layout — keyboard side = exactly the pre-C9.2 literals
    // (behavior identical), pad side = the fixed layout decided in the
    // chantier-9 plan.
    set(InputAction::Attack, Key::Count, MouseButton::Left,
        PadButton::RightTrigger);
    set(InputAction::Block, Key::Count, MouseButton::Right,
        PadButton::LeftTrigger);
    set(InputAction::DrawSheathe, Key::R, MouseButton::Count, PadButton::Y);
    set(InputAction::Sneak, Key::Ctrl, MouseButton::Count,
        PadButton::LeftStickClick);
    set(InputAction::SprintDodge, Key::Shift, MouseButton::Count,
        PadButton::B);
    set(InputAction::Jump, Key::Space, MouseButton::Count, PadButton::A);
    set(InputAction::Interact, Key::E, MouseButton::Count, PadButton::X);
    set(InputAction::Inventory, Key::I, MouseButton::Count,
        PadButton::DPadUp);
    set(InputAction::Journal, Key::J, MouseButton::Count, PadButton::Back);
    set(InputAction::WaitMenu, Key::T, MouseButton::Count,
        PadButton::DPadDown);
    set(InputAction::Map, Key::M, MouseButton::Count, PadButton::DPadLeft);
    set(InputAction::Pause, Key::Escape, MouseButton::Count,
        PadButton::Start);
    // É7: F was free on the keyboard; LB was free on the pad.
    set(InputAction::InteractAlt, Key::F, MouseButton::Count,
        PadButton::LeftShoulder);
}

bool ActionMap::down(const platform::Input& input,
                     InputAction action) const {
    const Binding& b = bindings[static_cast<size_t>(action)];
    return (b.key != Key::Count && input.isDown(b.key)) ||
           (b.mouse != MouseButton::Count && input.mouseDown(b.mouse)) ||
           (b.pad != PadButton::Count && input.padDown(b.pad));
}

bool ActionMap::pressed(const platform::Input& input,
                        InputAction action) const {
    const Binding& b = bindings[static_cast<size_t>(action)];
    return (b.key != Key::Count && input.wasPressed(b.key)) ||
           (b.mouse != MouseButton::Count && input.mousePressed(b.mouse)) ||
           (b.pad != PadButton::Count && input.padPressed(b.pad));
}

const Binding& ActionMap::binding(InputAction action) const {
    return bindings[static_cast<size_t>(action)];
}

void ActionMap::rebindKey(InputAction action, Key key) {
    for (Binding& b : bindings) {
        if (b.key == key) {
            b.key = Key::Count; // stolen from the previous owner
        }
    }
    bindings[static_cast<size_t>(action)].key = key;
}

void ActionMap::rebindMouse(InputAction action, MouseButton button) {
    for (Binding& b : bindings) {
        if (b.mouse == button) {
            b.mouse = MouseButton::Count;
        }
    }
    bindings[static_cast<size_t>(action)].mouse = button;
}

void ActionMap::rebindPad(InputAction action, PadButton button) {
    for (Binding& b : bindings) {
        if (b.pad == button) {
            b.pad = PadButton::Count;
        }
    }
    bindings[static_cast<size_t>(action)].pad = button;
}

// --- Names (settings.toml vocabulary + the options screen) -----------------

std::string_view actionName(InputAction action) {
    switch (action) {
    case InputAction::Attack:      return "attack";
    case InputAction::Block:       return "block";
    case InputAction::DrawSheathe: return "drawSheathe";
    case InputAction::Sneak:       return "sneak";
    case InputAction::SprintDodge: return "sprintDodge";
    case InputAction::Jump:        return "jump";
    case InputAction::Interact:    return "interact";
    case InputAction::Inventory:   return "inventory";
    case InputAction::Journal:     return "journal";
    case InputAction::WaitMenu:    return "wait";
    case InputAction::Map:         return "map";
    case InputAction::Pause:       return "pause";
    case InputAction::InteractAlt: return "interactAlt";
    case InputAction::Count:       break;
    }
    return "?";
}

std::optional<InputAction> parseAction(std::string_view name) {
    for (u8 i = 0; i < static_cast<u8>(InputAction::Count); ++i) {
        const auto action = static_cast<InputAction>(i);
        if (actionName(action) == name) {
            return action;
        }
    }
    return std::nullopt;
}

std::string_view keyName(Key key) {
    switch (key) {
    case Key::W: return "W";
    case Key::A: return "A";
    case Key::S: return "S";
    case Key::D: return "D";
    case Key::Up: return "Up";
    case Key::Down: return "Down";
    case Key::Left: return "Left";
    case Key::Right: return "Right";
    case Key::Space: return "Space";
    case Key::Enter: return "Enter";
    case Key::Escape: return "Escape";
    case Key::E: return "E";
    case Key::F: return "F";
    case Key::Q: return "Q";
    case Key::Shift: return "Shift";
    case Key::Ctrl: return "Ctrl";
    case Key::Num1: return "1";
    case Key::Num2: return "2";
    case Key::Num3: return "3";
    case Key::Num4: return "4";
    case Key::Num5: return "5";
    case Key::Tab: return "Tab";
    case Key::Backspace: return "Backspace";
    case Key::Delete: return "Delete";
    case Key::Home: return "Home";
    case Key::End: return "End";
    case Key::PageUp: return "PageUp";
    case Key::PageDown: return "PageDown";
    case Key::I: return "I";
    case Key::T: return "T";
    case Key::J: return "J";
    case Key::R: return "R";
    case Key::M: return "M";
    case Key::Alt: return "Alt"; // Option on macOS
    case Key::Count: break;
    }
    return "?";
}

std::optional<Key> parseKey(std::string_view name) {
    for (u16 i = 0; i < static_cast<u16>(Key::Count); ++i) {
        const auto key = static_cast<Key>(i);
        if (keyName(key) == name) {
            return key;
        }
    }
    return std::nullopt;
}

std::string_view mouseButtonName(MouseButton button) {
    switch (button) {
    case MouseButton::Left:   return "LMB";
    case MouseButton::Right:  return "RMB";
    case MouseButton::Middle: return "MMB";
    case MouseButton::Count:  break;
    }
    return "?";
}

std::optional<MouseButton> parseMouseButton(std::string_view name) {
    for (u8 i = 0; i < static_cast<u8>(MouseButton::Count); ++i) {
        const auto button = static_cast<MouseButton>(i);
        if (mouseButtonName(button) == name) {
            return button;
        }
    }
    return std::nullopt;
}

std::string_view padButtonName(PadButton button) {
    switch (button) {
    case PadButton::A: return "PadA";
    case PadButton::B: return "PadB";
    case PadButton::X: return "PadX";
    case PadButton::Y: return "PadY";
    case PadButton::LeftShoulder: return "LB";
    case PadButton::RightShoulder: return "RB";
    case PadButton::LeftTrigger: return "LT";
    case PadButton::RightTrigger: return "RT";
    case PadButton::Start: return "Start";
    case PadButton::Back: return "Back";
    case PadButton::DPadUp: return "DPadUp";
    case PadButton::DPadDown: return "DPadDown";
    case PadButton::DPadLeft: return "DPadLeft";
    case PadButton::DPadRight: return "DPadRight";
    case PadButton::LeftStickClick: return "LSClick";
    case PadButton::RightStickClick: return "RSClick";
    case PadButton::Count: break;
    }
    return "?";
}

std::optional<PadButton> parsePadButton(std::string_view name) {
    for (u8 i = 0; i < static_cast<u8>(PadButton::Count); ++i) {
        const auto button = static_cast<PadButton>(i);
        if (padButtonName(button) == name) {
            return button;
        }
    }
    return std::nullopt;
}

} // namespace game
