#include "game/Settings.hpp"

#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "engine/core/Log.hpp"
#include "engine/platform/Paths.hpp"

namespace game {

std::filesystem::path settingsPath() {
    return platform::executableDir() / "settings.toml";
}

void loadSettings(const std::filesystem::path& path, Settings& settings,
                  ActionMap& actions) {
    if (!std::filesystem::exists(path)) {
        return; // first run: defaults stand, the file appears on save
    }
    toml::parse_result parsed = toml::parse_file(path.string());
    if (!parsed) {
        LOG_WARN("settings.toml unreadable ({}): defaults stand",
                 std::string(parsed.error().description()));
        return;
    }
    const toml::table& root = parsed.table();
    const auto f32Of = [&root](const char* section, const char* key,
                               f32 fallback) {
        if (const toml::table* s = root[section].as_table()) {
            return static_cast<f32>(
                (*s)[key].value_or(static_cast<double>(fallback)));
        }
        return fallback;
    };
    settings.mouseSensitivity =
        f32Of("look", "mouseSensitivity", settings.mouseSensitivity);
    settings.stickSensitivity =
        f32Of("look", "stickSensitivity", settings.stickSensitivity);
    settings.stickDeadzone =
        f32Of("look", "deadzone", settings.stickDeadzone);
    if (const toml::table* look = root["look"].as_table()) {
        settings.invertLookY =
            (*look)["invertY"].value_or(settings.invertLookY);
    }
    settings.masterVolume =
        f32Of("audio", "masterVolume", settings.masterVolume);
    if (const toml::table* general = root["general"].as_table()) {
        settings.language =
            (*general)["language"].value_or(settings.language);
    }
    // Bindings: [bindings.<action>] key/mouse/pad = "<name>". Unknown
    // action or input names are skipped (stale file, older build).
    if (const toml::table* all = root["bindings"].as_table()) {
        for (const auto& [name, node] : *all) {
            const auto action = parseAction(name.str());
            const toml::table* entry = node.as_table();
            if (!action || !entry) {
                continue;
            }
            if (const auto text = (*entry)["key"].value<std::string>()) {
                if (const auto key = parseKey(*text)) {
                    actions.rebindKey(*action, *key);
                }
            }
            if (const auto text = (*entry)["mouse"].value<std::string>()) {
                if (const auto button = parseMouseButton(*text)) {
                    actions.rebindMouse(*action, *button);
                }
            }
            if (const auto text = (*entry)["pad"].value<std::string>()) {
                if (const auto button = parsePadButton(*text)) {
                    actions.rebindPad(*action, *button);
                }
            }
        }
    }
}

bool saveSettings(const std::filesystem::path& path,
                  const Settings& settings, const ActionMap& actions) {
    toml::table look;
    look.insert("mouseSensitivity",
                static_cast<double>(settings.mouseSensitivity));
    look.insert("stickSensitivity",
                static_cast<double>(settings.stickSensitivity));
    look.insert("invertY", settings.invertLookY);
    look.insert("deadzone", static_cast<double>(settings.stickDeadzone));
    toml::table audio;
    audio.insert("masterVolume",
                 static_cast<double>(settings.masterVolume));
    toml::table general;
    general.insert("language", std::string { settings.language });

    toml::table bindings;
    for (u8 i = 0; i < static_cast<u8>(InputAction::Count); ++i) {
        const auto action = static_cast<InputAction>(i);
        const Binding& b = actions.binding(action);
        toml::table entry;
        if (b.key != platform::Key::Count) {
            entry.insert("key", std::string { keyName(b.key) });
        }
        if (b.mouse != platform::MouseButton::Count) {
            entry.insert("mouse",
                         std::string { mouseButtonName(b.mouse) });
        }
        if (b.pad != platform::PadButton::Count) {
            entry.insert("pad", std::string { padButtonName(b.pad) });
        }
        bindings.insert(actionName(action), std::move(entry));
    }

    toml::table root;
    root.insert("look", std::move(look));
    root.insert("audio", std::move(audio));
    root.insert("general", std::move(general));
    root.insert("bindings", std::move(bindings));

    std::ofstream out(path);
    if (!out) {
        LOG_WARN("settings.toml not writable: {}", path.string());
        return false;
    }
    out << "# True Adventurer — machine preferences (not game data:\n"
           "# mods and saves never touch this file).\n\n"
        << root << "\n";
    return true;
}

} // namespace game
