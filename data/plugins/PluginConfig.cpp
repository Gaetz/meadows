#include "data/plugins/PluginConfig.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "engine/core/Log.hpp"

namespace data {

std::optional<PluginConfig> parsePluginConfigToml(
    std::string_view text, std::string_view sourceName) {
    toml::parse_result result = toml::parse(text);
    if (!result) {
        std::ostringstream error;
        error << result.error();
        LOG_ERROR("{}: TOML parse error: {}", sourceName, error.str());
        return std::nullopt;
    }
    PluginConfig config;
    const toml::array* plugins = result.table()["plugins"].as_array();
    if (!plugins) {
        return config; // empty config is valid (no plugins)
    }
    for (const toml::node& node : *plugins) {
        const toml::table* entry = node.as_table();
        if (!entry) {
            LOG_ERROR("{}: [[plugins]] entries must be tables", sourceName);
            return std::nullopt;
        }
        const auto file = (*entry)["file"].value<std::string>();
        if (!file || file->empty()) {
            LOG_ERROR("{}: [[plugins]] entry needs a 'file'", sourceName);
            return std::nullopt;
        }
        config.entries.push_back(
            { *file, (*entry)["enabled"].value<bool>().value_or(true) });
    }
    return config;
}

std::optional<PluginConfig> loadPluginConfigFile(
    const std::filesystem::path& path) {
    std::ifstream file { path, std::ios::binary };
    if (!file) {
        return std::nullopt; // absent is not an error (caller falls back)
    }
    std::ostringstream content;
    content << file.rdbuf();
    return parsePluginConfigToml(content.str(), path.filename().string());
}

str writePluginConfigToml(const PluginConfig& config) {
    std::ostringstream out;
    out << "# Plugin load order. Later plugins override earlier ones per\n"
           "# field (last writer wins). Managed by the in-game plugin\n"
           "# manager; hand-editing is fine too.\n";
    for (const PluginConfigEntry& entry : config.entries) {
        out << "\n[[plugins]]\n";
        out << "file = \"" << entry.file << "\"\n";
        if (!entry.enabled) {
            out << "enabled = false\n";
        }
    }
    return out.str();
}

PluginConfig defaultConfigFromDirectory(
    const std::filesystem::path& directory) {
    PluginConfig config;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator { directory, ec }) {
        if (!entry.is_regular_file() ||
            entry.path().extension() != ".toml") {
            continue;
        }
        const str filename = entry.path().filename().string();
        if (filename == "plugins.toml") {
            continue; // the config file itself is not a plugin
        }
        config.entries.push_back({ filename, true });
    }
    std::sort(config.entries.begin(), config.entries.end(),
              [](const PluginConfigEntry& a, const PluginConfigEntry& b) {
                  return a.file < b.file;
              });
    return config;
}

PluginStack loadPluginStack(const std::filesystem::path& directory,
                            const PluginConfig& config,
                            const FormTypeRegistry& types) {
    PluginStack stack;
    for (const PluginConfigEntry& entry : config.entries) {
        if (!entry.enabled) {
            continue;
        }
        // An absent file is not an error: optional layers (editor exports,
        // not-yet-installed mods) list themselves before they exist.
        if (!std::filesystem::exists(directory / entry.file)) {
            LOG_INFO("plugin '{}' not present — skipped", entry.file);
            stack.skipped.push_back(entry.file);
            continue;
        }
        auto plugin = loadPluginFile(directory / entry.file, types);
        if (!plugin) {
            stack.errors.push_back("failed to load " + entry.file);
            continue; // resolve proceeds without it — never fatal (§5)
        }
        stack.plugins.push_back(std::move(*plugin));
    }
    return stack;
}

vector<const Plugin*> pointersOf(const PluginStack& stack) {
    vector<const Plugin*> pointers;
    pointers.reserve(stack.plugins.size());
    for (const Plugin& plugin : stack.plugins) {
        pointers.push_back(&plugin);
    }
    return pointers;
}

} // namespace data
