#pragma once

#include <functional>
#include <optional>

#include "engine/core/Defines.hpp"
#include "game/InputActions.hpp"

namespace audio {
class AudioSystem;
}
namespace data {
class TextTable;
}
namespace ui {
class UiSystem;
}

namespace game {

struct Settings;
class ScreenStack;

// Chantier 9 C9.4 — the OPTIONS screen: look settings, master volume and
// the full bindings table with press-to-rebind capture. Values live in
// game::Settings + the ActionMap (machine preferences, deliberately NOT
// Forms); every change is written to settings.toml immediately and
// applied live — the file stays the single source of truth.
struct OptionsContext {
    Settings& settings;
    ActionMap& actions;
    platform::Input& input;    // deadzone application + the capture scan
    ::ui::UiSystem& ui;        // the "options" data model
    ScreenStack& screenStack;  // Back pops to the menu underneath
    audio::AudioSystem& audio; // master volume across the fixed buses
    const data::TextTable& texts; // C9.5: On/Off + action labels
    // C9.5: the scene's LIVE language application — rebuild the TextTable
    // from the re-gated plugin stack + relocalize every loaded document
    // (settings.language was already flipped and saved when this fires).
    std::function<void()> applyLanguage;
};

// The capture verdict for one polled frame — PURE (doctested). Priority:
// Escape cancels, whatever else landed the same frame; otherwise the
// first pressed key wins over a mouse button over a pad button (one
// device slot rebinds per capture; the others keep their binding).
struct CaptureResult {
    enum class Kind : u8 { None, Cancel, Key, Mouse, Pad };
    Kind kind { Kind::None };
    platform::Key key { platform::Key::Count };
    platform::MouseButton mouse { platform::MouseButton::Count };
    platform::PadButton pad { platform::PadButton::Count };
};

inline CaptureResult decideCapture(
    std::optional<platform::Key> keyPressed,
    std::optional<platform::MouseButton> mousePressed,
    std::optional<platform::PadButton> padPressed) {
    if (keyPressed == platform::Key::Escape) {
        return { .kind = CaptureResult::Kind::Cancel };
    }
    if (keyPressed) {
        return { .kind = CaptureResult::Kind::Key, .key = *keyPressed };
    }
    if (mousePressed) {
        return { .kind = CaptureResult::Kind::Mouse,
                 .mouse = *mousePressed };
    }
    if (padPressed) {
        return { .kind = CaptureResult::Kind::Pad, .pad = *padPressed };
    }
    return {};
}

// The H6 audio facade has no engine-level master knob — its contract is
// per-bus volumes over a FIXED bus set (engine/audio/Audio.hpp: "the
// options screen binds to setBusVolume"). One write across the five
// buses IS the master volume: play() only routes through them.
void applyMasterVolume(audio::AudioSystem& audio, f32 volume);

class OptionsController {
public:
    // menuAction("options"): fresh model values, then show the screen.
    void open(const OptionsContext& ctx);

    // The "options" data-model events (steppers, invert, rebind, back).
    void handleEvent(const OptionsContext& ctx, const str& event,
                     const vector<str>& args);

    // True while a rebind capture is armed — the scene gates Tab/Pause
    // and the C9.3 pad->UI routing on this, so the captured press (B,
    // Start, Escape...) reaches nothing else.
    bool capturing() const { return capturing_.has_value(); }
    // Per-frame poll while armed: the first pressed key (Escape =
    // cancel) / mouse / pad button rebinds the captured action, saves
    // settings.toml and rebuilds the table (steal-on-rebind can blank
    // ANOTHER row's cell).
    void updateCapture(const OptionsContext& ctx);

    void reset() { capturing_.reset(); } // onExit: drop an armed capture

private:
    void pushModel(const OptionsContext& ctx);
    void applyAndSave(const OptionsContext& ctx);

    std::optional<InputAction> capturing_;
};

} // namespace game
