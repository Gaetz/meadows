#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/Record.hpp"

namespace data {

// Parses the human-readable plugin format (TOML, §3 on-disk formats).
//
// Error policy — mod data is untrusted (§5):
//   - malformed TOML / missing [plugin] header: error, returns nullopt;
//   - unknown record type, bad guid, unknown field, kind mismatch:
//     logged warning, the record/field is skipped, loading continues.
//
// `sourceName` only labels log messages (file name, test name...).
std::optional<Plugin> parsePluginToml(std::string_view text,
                                      const FormTypeRegistry& types,
                                      std::string_view sourceName);

std::optional<Plugin> loadPluginFile(const std::filesystem::path& path,
                                     const FormTypeRegistry& types);

} // namespace data
