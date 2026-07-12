#pragma once

#include <filesystem>

#include "engine/core/Defines.hpp"
#include "game/InputActions.hpp"

// Chantier 9 C9.2 — machine preferences. Deliberately NOT a Form and NOT
// a save layer: this is per-machine state (look feel, bindings, language,
// volume) that must survive independently of any game data or save —
// a plain settings.toml beside saves/.

namespace game {

struct Settings {
    f32 mouseSensitivity { 1.0f }; // multiplier over the base look sens
    f32 stickSensitivity { 2.2f }; // rad/s of look at full deflection [cpp-tuning]
    bool invertLookY { false };
    f32 stickDeadzone { 0.15f };
    f32 masterVolume { 1.0f };     // applied with the options screen (C9.4)
    str language { "fr" };         // which language pack plugin is on (C9.5)
};

std::filesystem::path settingsPath(); // executableDir()/settings.toml

// Missing file / missing field / unknown name = the defaults (and the
// ActionMap's default layout) stand — a stale or hand-edited file can
// only override what it actually names.
void loadSettings(const std::filesystem::path& path, Settings& settings,
                  ActionMap& actions);
bool saveSettings(const std::filesystem::path& path,
                  const Settings& settings, const ActionMap& actions);

} // namespace game
