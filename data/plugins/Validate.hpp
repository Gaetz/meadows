#pragma once

#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/Record.hpp"
#include "data/plugins/Resolver.hpp"

// Plugin/mod lint: one
// reusable pass over a load order — resolve through THE §5 resolver (its
// report already carries orphan patches, dependency violations and the
// per-field conflicts), then a reflection-driven sweep for DANGLING GUID
// references: any Guid field of any resolved form pointing at something
// no plugin creates and no plugin's asset list declares. Reflection is
// the keystone (§2.3): a new Form type is linted for free.
//
// Conflicts are NOT errors — same-field layering is the design (§5), the
// synthesis tool arbitrates them. Errors = what a shipped mod must fix:
// load failures, orphan patches, dependency violations, dangling guids.
// Consumers: `cooker validate` (CLI, exit code for mod CI); the editor
// PluginsPanel can reuse the same report later.

namespace data {

struct DanglingReference {
    core::Guid form;   // the referencing form
    str typeName;      // its type
    str fieldName;     // the Guid field
    core::Guid target; // the guid nothing creates or declares
};

struct DeprecatedWrite {
    core::Guid form; // the record writing it
    str typeName;
    str fieldName;   // flagged reflect::Deprecated
};

// A resolved Str/I32 field holding a value outside its reflect::EnumTable
// (a record that bypassed the loader's check, e.g. cooked binary).
struct VocabularyError {
    core::Guid form;
    str typeName;
    str fieldName;
    str value;
};

struct ValidationReport {
    ResolveReport resolve;
    vector<DanglingReference> danglingRefs;
    vector<VocabularyError> vocabularyErrors;
    // Warnings, not errors: the values still apply, the authoring should
    // move to the field's replacement.
    vector<DeprecatedWrite> deprecatedWrites;

    bool hasErrors() const {
        return resolve.orphanPatches > 0 ||
               resolve.dependencyViolations > 0 || !danglingRefs.empty() ||
               !vocabularyErrors.empty();
    }
};

// Validates `loadOrder` (same order semantics as resolve). Known targets =
// every resolved form guid + every asset guid declared by any plugin in
// the order + `runtimeProvided` (guids the ENGINE injects at runtime —
// procedural meshes; the cooker passes game::runtimeMeshGuids()).
// Deterministic output (form handle order, field order).
ValidationReport validatePlugins(const vector<const Plugin*>& loadOrder,
                                 const FormTypeRegistry& types,
                                 const vector<core::Guid>& runtimeProvided = {});

} // namespace data
