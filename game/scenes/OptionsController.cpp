#include "game/scenes/OptionsController.hpp"

#include <cstdio>
#include <string_view>

#include <glm/glm.hpp>

#include "data/forms/LocForms.hpp" // TextTable
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

// The languages the toggle cycles through. Language NAMES stay in their
// own language on purpose (the one label a lost player must always read).
constexpr const char* kLanguages[][2] = { { "en", "English" },
                                          { "fr", "Francais" } };

str languageLabel(std::string_view code) {
    for (const auto& [name, label] : kLanguages) {
        if (code == name) {
            return label;
        }
    }
    return str { code }; // a modded pack shows its raw code
}

str nextLanguage(std::string_view code) {
    constexpr size_t count = sizeof(kLanguages) / sizeof(kLanguages[0]);
    for (size_t i = 0; i < count; ++i) {
        if (code == kLanguages[i][0]) {
            return kLanguages[(i + 1) % count][0];
        }
    }
    return kLanguages[0][0]; // unknown code: back to the base language
}

// The +/- steppers of the options screen: one row per f32 setting —
// the id adjust('<id>', '+'|'-') emits, the field, the step and its
// clamp, the model string that shows it and that string's precision.
struct Stepper {
    std::string_view id;
    f32 Settings::* field;
    f32 step;
    f32 min;
    f32 max;
    const char* textId;
    int decimals;
};
constexpr Stepper kSteppers[] = {
    { "mouse", &Settings::mouseSensitivity, 0.1f, 0.1f, 5.0f,
      "mouseSensText", 1 },
    { "stick", &Settings::stickSensitivity, 0.2f, 0.5f, 8.0f,
      "stickSensText", 1 },
    { "deadzone", &Settings::stickDeadzone, 0.05f, 0.0f, 0.5f,
      "deadzoneText", 2 },
    { "volume", &Settings::masterVolume, 0.1f, 0.0f, 1.0f, "volumeText",
      1 },
};

str formatFixed(f32 value, int decimals) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
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
    for (const Stepper& stepper : kSteppers) {
        ctx.ui.setString("options", stepper.textId,
                         formatFixed(s.*stepper.field, stepper.decimals));
    }
    ctx.ui.setString("options", "invertText",
                     ctx.texts.get(s.invertLookY ? "ui.options.on"
                                                 : "ui.options.off"));
    ctx.ui.setString("options", "languageText", languageLabel(s.language));
    ctx.ui.setBool("options", "capturing", capturing_.has_value());

    vector<::ui::UiRow> rows;
    for (u8 i = 0; i < static_cast<u8>(InputAction::Count); ++i) {
        const auto action = static_cast<InputAction>(i);
        const Binding& b = ctx.actions.binding(action);
        ::ui::UiRow row;
        row.id = actionName(action);
        // The human label is loc data keyed on the STABLE action
        // name (ui.options.action.attack...) — one key per action, no
        // per-language C++.
        row.c0 = ctx.texts.get(str { "ui.options.action." } +
                               str { actionName(action) });
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
        for (const Stepper& stepper : kSteppers) {
            if (args[0] == stepper.id) {
                f32& value = s.*stepper.field;
                value = glm::clamp(value + dir * stepper.step, stepper.min,
                                   stepper.max);
                break;
            }
        }
        applyAndSave(ctx);
    } else if (event == "toggleInvert") {
        ctx.settings.invertLookY = !ctx.settings.invertLookY;
        applyAndSave(ctx);
    } else if (event == "toggleLanguage") {
        // Flip + persist, then the scene rebuilds the TextTable from
        // the re-gated plugin stack and relocalizes every open document —
        // pushModel LAST so this screen's C++-pushed strings follow too.
        ctx.settings.language = nextLanguage(ctx.settings.language);
        saveSettings(settingsPath(), ctx.settings, ctx.actions);
        if (ctx.applyLanguage) {
            ctx.applyLanguage();
        }
        pushModel(ctx);
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
