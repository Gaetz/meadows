#pragma once

#include <filesystem>
#include <optional>

#include "data/plugins/PluginLoader.hpp"

// Plugin load-order configuration (horizontal pass H1) — the "internal
// plugin configuration" the demo wants: which plugin files load, in which
// order, enabled or not. The game AND the editor/plugin-manager UI go
// through this; the manager rewrites the file when the user reorders.
//
// plugins.toml format:
//   [[plugins]]
//   file = "base.toml"
//   enabled = true
//   [[plugins]]
//   file = "landscape.toml"
//
// HOW TO FILL (post-7/07): the PluginsPanel (H2) edits a PluginConfig in
// memory, calls writePluginConfigToml to persist, and the game re-resolves
// on next launch. Dependency checking (Plugin::dependencies guids) belongs
// in the panel: warn when a dependency is missing/disabled/after.

namespace data {

struct PluginConfigEntry {
    str file;              // relative to the plugins directory
    bool enabled { true };
};

struct PluginConfig {
    vector<PluginConfigEntry> entries; // load order = vector order
};

std::optional<PluginConfig> parsePluginConfigToml(std::string_view text,
                                                  std::string_view sourceName);
std::optional<PluginConfig> loadPluginConfigFile(
    const std::filesystem::path& path);
str writePluginConfigToml(const PluginConfig& config);

// Migration path: no plugins.toml yet -> every *.toml in the directory,
// sorted by filename (deterministic default order).
PluginConfig defaultConfigFromDirectory(
    const std::filesystem::path& directory);

// Loads every enabled entry. Files that exist but fail to load land in
// `errors`; listed-but-absent files land in `skipped` (optional layers —
// editor exports, not-yet-installed mods — may list themselves before they
// exist). The returned plugins are self-owning; feed pointersOf() to
// data::resolve().
struct PluginStack {
    vector<Plugin> plugins;
    vector<str> errors;
    vector<str> skipped;
};
PluginStack loadPluginStack(const std::filesystem::path& directory,
                            const PluginConfig& config,
                            const FormTypeRegistry& types);
vector<const Plugin*> pointersOf(const PluginStack& stack);

} // namespace data
