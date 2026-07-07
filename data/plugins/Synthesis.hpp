#pragma once

#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/Record.hpp"

// §5.1 — conflict arbitration. A synthesis patch is an ORDINARY plugin
// loaded LAST: no special format, no new engine mechanism — the answer to
// "force this value" is always one more layer (§2.4). A tool reads the
// resolver's conflict report (FieldConflict now carries each writer's
// value), lets the user pick a winner (or a custom value) per conflicted
// field, and this module emits the result through the existing writer.

namespace data {

class FormDatabase;

// One arbitrated field: the value the user chose for one (form, field).
struct SynthesisChoice {
    core::Guid formId;
    u32 typeId { 0 };
    u32 fieldId { 0 };
    str fieldName;        // for the provenance header (readability)
    reflect::Value value; // the arbitrated value
    str provenance;       // plugin name it came from; "" = custom value
};

// Groups the choices into per-form PATCH records (creates = false),
// sorted by guid (determinism). `dependencies` should list the arbitrated
// plugins so load-order validation can keep the patch last.
Plugin makeSynthesisPatch(const core::Guid& pluginId, const str& name,
                          const vector<SynthesisChoice>& choices,
                          const vector<core::Guid>& dependencies);

// writePluginToml plus a provenance header (`# <form>.<field> from: ...`)
// so the tool can re-apply choices after a modlist update and flag stale
// picks (§5.1). `database` (optional) turns form guids into editorIds.
str writeSynthesisToml(const Plugin& plugin, const FormTypeRegistry& types,
                       const vector<SynthesisChoice>& choices,
                       const FormDatabase* database);

} // namespace data
