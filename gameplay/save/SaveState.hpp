#pragma once

#include "data/plugins/Record.hpp"
#include "data/plugins/RecordDiff.hpp"
#include "engine/ecs/World.hpp" // ecs::Entity (flecs name confined to meadows-ecs)
#include "gameplay/save/SaveForms.hpp"

// Capture/apply between live actor state and save records.
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

// Materializes a typed Form from a raw Record (defaults + the record's
// fields) — how the PENDING in-memory layer turns captured records back
// into applicable state without a resolver pass.
template<typename T>
T formFromRecord(const data::Record& record) {
    T form;
    const reflect::TypeInfo& type = T::staticTypeInfo();
    for (const auto& [fieldId, value] : record.fields) {
        if (const reflect::FieldInfo* field = type.findField(fieldId)) {
            field->set(&form, value);
        }
    }
    form.id = record.formId;
    return form;
}

// Builds a `creates = true` Record from a filled Form instance, carrying
// only the OWN fields that differ from the type's C++ defaults (the same §5
// rule as EditSession's export — both go through data::diffToRecord). Own
// fields only: inherited id/editorId stay out, the record guid is the
// identity. Transient fields are skipped.
template<typename T>
data::Record createRecord(const T& filled, const core::Guid& id) {
    static const T kDefaults {};
    const reflect::TypeInfo& type = T::staticTypeInfo();
    data::Record record;
    record.formId = id;
    record.typeId = type.id;
    record.creates = true;
    data::diffToRecord(type, &filled, &kDefaults, record,
                       /*includeInherited=*/false);
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

// Captures one actor entity's full persistent state (the §6 contract:
// component BASE values + active durational effects; currents are never
// stored) as child records of `refGuid`: one SavedStatsForm (also the
// "was captured" sentinel that suppresses loadout re-rolls), plus
// SavedEffect/SavedItem/SavedInjury rows. Missing components are simply
// skipped. Deterministic record guids; items sorted by item guid (§8).
vector<data::Record> captureActor(ecs::Entity entity,
                                  const core::Guid& refGuid,
                                  const GameplayTagRegistry& registry);

// The inverse: writes the SavedStatsForm fields back into the entity's
// components (by reflection name matching), restores the effect rows,
// refills Inventory/Injuries/Equipment. Call strictly AFTER
// initializeActorStats (which resets vitals and clears State.Dead), with
// the same records the capture produced (resolved or pending). Ends with
// a current recompute + updateLifeState so a dead actor loads dead.
struct SavedActorRecords {
    const SavedStatsForm* stats { nullptr };
    vector<const SavedEffectForm*> effects;
    vector<const SavedItemForm*> items;
    vector<const SavedInjuryForm*> injuries;
    vector<const SavedAbilityForm*> abilities; // granted abilities
    vector<const SavedSkillForm*> skills;      // skills-by-use
    vector<const SavedBountyForm*> bounties;   // per-faction crime
};
void applySavedState(ecs::Entity entity, const SavedActorRecords& saved,
                     const GameplayTagRegistry& registry);

// Convenience: gathers an actor's saved child records from a resolved
// database (returns stats = nullptr when the actor was never captured).
SavedActorRecords savedRecordsFor(const data::FormDatabase& forms,
                                  const core::Guid& refGuid);

} // namespace gameplay
