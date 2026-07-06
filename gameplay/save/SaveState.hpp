#pragma once

#include <flecs.h>

#include "data/plugins/Record.hpp"
#include "gameplay/save/SaveForms.hpp"

// Capture/apply between live actor state and save records (chantier 5).
// Headless by construction: everything here reads/writes components and
// Records — no scene, no renderer. The scene only decides WHEN (on cell
// unload, on save, after spawn).

namespace data {
class FormDatabase;
}

namespace gameplay {

struct AttributeSet;
struct AbilitySystem;
class GameplayTagRegistry;

// Copies every OWN field of `srcType` whose name (fnv1a id) also exists
// on `dstType`, through reflection. The generic bridge between the stat
// components and SavedStatsForm (same field names on both sides); works
// in both directions.
void copyMatchingFields(const reflect::TypeInfo& srcType, const void* src,
                        const reflect::TypeInfo& dstType, void* dst);

// Builds a `creates = true` Record from a filled Form instance, carrying
// only the OWN fields that differ from the type's C++ defaults (the
// resolver applies defaults as layer zero — EditSession's export
// semantics). Transient fields are skipped.
template<typename T>
data::Record createRecord(const T& filled, const core::Guid& id) {
    static const T kDefaults {};
    const reflect::TypeInfo& type = T::staticTypeInfo();
    data::Record record;
    record.formId = id;
    record.typeId = type.id;
    record.creates = true;
    for (const reflect::FieldInfo& field : type.fields) {
        if (field.flags & reflect::Transient) {
            continue;
        }
        reflect::Value value = field.get(&filled);
        if (!(value == field.get(&kDefaults))) {
            record.fields.emplace(field.id, std::move(value));
        }
    }
    return record;
}

// Re-creates one ACTIVE effect row from its saved mirror: pushes the
// ActiveEffect, re-adds the granted tag (ref-counted), allocates a fresh
// effectId. Does NOT recompute currents — the caller recomputes once
// after restoring every row (§6: currents are derived).
void restoreActiveEffect(AbilitySystem& system, const SavedEffectForm& row,
                         const GameplayTagRegistry& registry);

// The mirror of restoreActiveEffect: one saved record per active row.
// `parent` = the actor's ReferenceForm guid; guids are deterministic
// (Guid::combine of a namespace, the parent and the row index) so
// re-saving is idempotent. Rows for expired effects are never emitted.
vector<data::Record> captureActiveEffects(const AbilitySystem& system,
                                          const core::Guid& parent,
                                          const GameplayTagRegistry& registry);

} // namespace gameplay
