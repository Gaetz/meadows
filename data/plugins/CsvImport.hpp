#pragma once

#include <string_view>

#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/Record.hpp"
#include "engine/core/Result.hpp"

namespace data {

// Generic CSV -> plugin bridge: one row =
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
//
// PATCH mode (language packs): pass `patchTarget` = the plugin guid
// whose rows this sheet overrides. Rows then derive their identity from
// (patchTarget, editorId) — the TARGET plugin's csvRowGuid — and become
// ordinary §5 patch records (new = false) carrying only the value columns
// (the editorId column is identity-only, not re-written). An explicit
// `form` column still wins, as in create mode.
core::Result<Plugin> importCsv(std::string_view csv,
                               const reflect::TypeInfo& type,
                               const core::Guid& pluginId,
                               std::string_view sourceName,
                               const core::Guid& patchTarget = {});

// The deterministic row identity used above, exposed for tools/tests:
// a name-derived guid combined with the plugin's (stable across runs).
core::Guid csvRowGuid(const core::Guid& pluginId, std::string_view editorId);

} // namespace data
