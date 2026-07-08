#pragma once

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/Record.hpp"

namespace data {

// One write in a conflict — 8.5: the value rides along so the synthesis
// tool (§5.1) can SHOW what each plugin wanted, not just who wrote.
struct FieldWrite {
    str plugin;           // writer plugin name
    reflect::Value value; // what it wrote
    bool operator==(const FieldWrite&) const = default;
};

// A field written by two or more plugins. This includes the normal
// "mod overrides base game" case on purpose: the resolver reports raw
// facts, filtering (e.g. hiding base-game writers) is a presentation
// concern of the conflict view.
struct FieldConflict {
    core::Guid formId;
    str typeName;
    str fieldName;
    u32 typeId { 0 };  // 8.5: what a synthesis patch Record needs
    u32 fieldId { 0 };
    vector<FieldWrite> writers; // in apply order; the last one won
};

struct ResolveReport {
    u32 formsCreated { 0 };
    u32 recordsApplied { 0 };
    u32 recordsSkipped { 0 };  // type mismatch, unknown type...
    u32 orphanPatches { 0 };   // patches to a guid no plugin creates
    u32 dependencyViolations { 0 }; // declared dep missing or loaded after
                                    // its dependent (audit U7-6)
    vector<FieldConflict> conflicts;

    bool hasConflicts() const { return !conflicts.empty(); }
};

// THE §5 resolution, the single layering mechanism of the project (§2.4):
// base game, every mod, and later the save are all just ordered layers
// here — the resolver does not know which is which.
//
// For each form: C++ defaults (layer zero), then every record that touches
// it, strictly in load order — last writer wins *per field*. The create
// record establishes existence and type; a duplicate create degrades to a
// patch (warn). Patches to forms that nothing creates are counted and
// dropped. Deterministic: same inputs, same database, same report.
ResolveReport resolve(const vector<const Plugin*>& loadOrder,
                      const FormTypeRegistry& types, FormDatabase& outDatabase);

} // namespace data
