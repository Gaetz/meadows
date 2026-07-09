#pragma once

#include <filesystem>
#include <string_view>

#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/Record.hpp"
#include "engine/core/Result.hpp"

namespace data {

// Parses the human-readable plugin format (TOML, §3 on-disk formats).
//
// Error policy — mod data is untrusted (§5):
//   - malformed TOML / missing [plugin] header: fatal for THIS plugin —
//     the Result carries the reason (U1-03: the plugin panel / cooker /
//     save loader can show WHY, not just "failed");
//   - unknown record type, bad guid, unknown field, kind mismatch:
//     logged warning, the record/field is skipped, loading continues.
//
// `sourceName` only labels log messages (file name, test name...).
core::Result<Plugin> parsePluginToml(std::string_view text,
                                     const FormTypeRegistry& types,
                                     std::string_view sourceName);

core::Result<Plugin> loadPluginFile(const std::filesystem::path& path,
                                    const FormTypeRegistry& types);

} // namespace data
