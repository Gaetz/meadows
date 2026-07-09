#pragma once

#include <string_view>

#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/Record.hpp"
#include "engine/core/Result.hpp"

namespace data {

// Generic CSV -> plugin bridge (U4-11 + the mass-import intent): one row =
// one NEW record of `type`, columns = reflected field names (header row).
// The output is an ORDINARY plugin (§5): it layers, diffs and patches like
// any hand-written TOML. First uses: localisation string tables, bulk item
// lists authored in a spreadsheet.
//
// Identity rules (the part that must never surprise):
//   - a `form` column (guid) gives a row its EXPLICIT identity;
//   - otherwise the guid is DERIVED deterministically from
//     (pluginId, editorId) — re-importing the same sheet yields the same
//     guids, so cross-references and §5 patches stay valid. Renaming an
//     editorId therefore CHANGES the row's identity (documented tradeoff);
//   - a row with neither `form` nor `editorId` is skipped with a warning.
//
// Field policy mirrors the TOML loader (§5, mod data is untrusted):
// unknown column / transient field / unparsable value = logged warning,
// the cell is skipped, the import continues. A malformed header is fatal
// (the Result carries the reason).
core::Result<Plugin> importCsv(std::string_view csv,
                               const reflect::TypeInfo& type,
                               const core::Guid& pluginId,
                               std::string_view sourceName);

// The deterministic row identity used above, exposed for tools/tests:
// a name-derived guid combined with the plugin's (stable across runs).
core::Guid csvRowGuid(const core::Guid& pluginId, std::string_view editorId);

} // namespace data
