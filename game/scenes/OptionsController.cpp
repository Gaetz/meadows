#include "game/scenes/OptionsController.hpp"

#include <cstdio>

#include <glm/glm.hpp>

#include "engine/audio/Audio.hpp"
#include "engine/ui/UiSystem.hpp"
#include "game/ScreenStack.hpp"
#include "game/Settings.hpp"

namespace game {

namespace {

// The fixed mixer buses (engine/audio/Audio.hpp: sfx|music|voice|
// ambient|ui) — the master volume is one write across all of them.
constexpr const char* kBuses[] = { "sfx", "music", "voice", "ambient",
                                   "ui" };

// Human labels for the bindings table (actionName() is the settings.toml
// vocabulary — stable and lowercase; this is presentation only).
const char* actionLabel(InputAction action) {
    switch (action) {
    case InputAction::Attack:      return "Attack";
    case InputAction::Block:       return "Block";
    case InputAction::DrawSheathe: return "Draw / sheathe";
    case InputAction::Sneak:       return "Sneak";
    case InputAction::SprintDodge: return "Sprint / dodge";
    case InputAction::Jump:        return "Jump";
    case InputAction::Interact:    return "Interact";
    case InputAction::Inventory:   return "Inventory";
    case InputAction::Journal:     return "Journal";
    case InputAction::WaitMenu:    return "Wait";
    case InputAction::Map:         return "Map";
    case InputAction::Pause:       return "Pause";
    case InputAction::Count:       break;
    }
    return "?";
}

str fmt1(f32 value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.1f", value);
    return text;
}

str fmt2(f32 value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.2f", value);
    return text;
}

str cell(std::string_view name, bool bound) {
    return bound ? str { name } : str { "-" };
}

} // namespace

void applyMasterVolume(audio::AudioSystem& audio, f32 volume) {
    for (const char* bus : kBuses) {
        audio.setBusVolume(bus, volume);
    }
}

void OptionsController::open(const OptionsContext& ctx) {
    capturing_.reset();
    pushModel(ctx);
    ctx.screenStack.show("options");
}

void OptionsController::pushModel(const OptionsContext& ctx) {
    const Settings& s = ctx.settings;
    ctx.ui.setString("options", "mouseSensText", fmt1(s.mouseSensitivity));
    ctx.ui.setString("options", "stickSensText", fmt1(s.stickSensitivity));
    ctx.ui.setString("options", "deadzoneText", fmt2(s.stickDeadzone));
    ctx.ui.setString("options", "volumeText", fmt1(s.masterVolume));
    ctx.ui.setString("options", "invertText", s.invertLookY ? "On" : "Off");
    ctx.ui.setBool("options", "capturing", capturing_.has_value());

    vector<::ui::UiRow> rows;
    for (u8 i = 0; i < static_cast<u8>(InputAction::Count); ++i) {
        const auto action = static_cast<InputAction>(i);
        const Binding& b = ctx.actions.binding(action);
        ::ui::UiRow row;
        row.id = actionName(action);
        row.c0 = actionLabel(action);
        row.c1 = cell(keyName(b.key), b.key != platform::Key::Count);
        row.c2 = cell(mouseButtonName(b.mouse),
                      b.mouse != platform::MouseButton::Count);
        row.c3 = cell(padButtonName(b.pad),
                      b.pad != platform::PadButton::Count);
        if (capturing_ == action) {
            row.tag = "capturing"; // highlighted; the banner says why
        }
        rows.push_back(std::move(row));
    }
    ctx.ui.setRows("options", std::move(rows));
}

void OptionsController::applyAndSave(const OptionsContext& ctx) {
    // Live application: the deadzone feeds the input layer, the volume
    // the buses; sensitivities/invert are read from ctx.settings each
    // frame by the PlayerController — nothing to re-apply there.
    ctx.input.setStickDeadzone(ctx.settings.stickDeadzone);
    applyMasterVolume(ctx.audio, ctx.settings.masterVolume);
    saveSettings(settingsPath(), ctx.settings, ctx.actions);
    pushModel(ctx);
}

void OptionsController::handleEvent(const OptionsContext& ctx,
                                    const str& event,
                                    const vector<str>& args) {
    if (event == "adjust" && args.size() >= 2) {
        Settings& s = ctx.settings;
        const f32 dir = args[1] == "+" ? 1.0f : -1.0f;
        if (args[0] == "mouse") {
            s.mouseSensitivity =
                glm::clamp(s.mouseSensitivity + dir * 0.1f, 0.1f, 5.0f);
        } else if (args[0] == "stick") {
            s.stickSensitivity =
                glm::clamp(s.stickSensitivity + dir * 0.2f, 0.5f, 8.0f);
        } else if (args[0] == "deadzone") {
            s.stickDeadzone =
                glm::clamp(s.stickDeadzone + dir * 0.05f, 0.0f, 0.5f);
        } else if (args[0] == "volume") {
            s.masterVolume =
                glm::clamp(s.masterVolume + dir * 0.1f, 0.0f, 1.0f);
        }
        applyAndSave(ctx);
    } else if (event == "toggleInvert") {
        ctx.settings.invertLookY = !ctx.settings.invertLookY;
        applyAndSave(ctx);
    } else if (event == "rebind" && !args.empty()) {
        if (const auto action = parseAction(args[0])) {
            capturing_ = *action;
            pushModel(ctx); // the banner + row highlight show up now
        }
    } else if (event == "optionsBack") {
        capturing_.reset();
        ctx.screenStack.closeTop();
    }
}

void OptionsController::updateCapture(const OptionsContext& ctx) {
    if (!capturing_) {
        return;
    }
    // Scan the polled snapshot for this frame's first press per device
    // class; the pure decision (Escape cancels, key > mouse > pad) is
    // decideCapture — doctested in InputActionsTest.
    std::optional<platform::Key> key;
    for (u16 i = 0; i < static_cast<u16>(platform::Key::Count); ++i) {
        if (ctx.input.wasPressed(static_cast<platform::Key>(i))) {
            key = static_cast<platform::Key>(i);
            break;
        }
    }
    std::optional<platform::MouseButton> mouse;
    for (u8 i = 0; i < static_cast<u8>(platform::MouseButton::Count); ++i) {
        if (ctx.input.mousePressed(static_cast<platform::MouseButton>(i))) {
            mouse = static_cast<platform::MouseButton>(i);
            break;
        }
    }
    std::optional<platform::PadButton> pad;
    for (u8 i = 0; i < static_cast<u8>(platform::PadButton::Count); ++i) {
        if (ctx.input.padPressed(static_cast<platform::PadButton>(i))) {
            pad = static_cast<platform::PadButton>(i);
            break;
        }
    }
    const CaptureResult result = decideCapture(key, mouse, pad);
    switch (result.kind) {
    case CaptureResult::Kind::None:
        return; // still waiting
    case CaptureResult::Kind::Cancel:
        capturing_.reset();
        pushModel(ctx);
        return;
    case CaptureResult::Kind::Key:
        ctx.actions.rebindKey(*capturing_, result.key);
        break;
    case CaptureResult::Kind::Mouse:
        ctx.actions.rebindMouse(*capturing_, result.mouse);
        break;
    case CaptureResult::Kind::Pad:
        ctx.actions.rebindPad(*capturing_, result.pad);
        break;
    }
    capturing_.reset();
    // Save + rebuild the WHOLE table: steal-on-rebind can have blanked
    // another action's cell.
    applyAndSave(ctx);
}

} // namespace game
