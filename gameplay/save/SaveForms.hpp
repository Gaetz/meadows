#pragma once

#include <string_view>
#include <variant>

#include "data/forms/Form.hpp"
#include "engine/core/Assert.hpp"

// The save-game record types. A SAVE IS AN ORDINARY PLUGIN
// (§2.4/§5 — the non-negotiable): these Forms are its records, resolved by
// the same resolver as every mod, written by the same TomlWriter, cooked
// by the same cooker. Per-actor state rides as CHILD records keyed by
// `parent` = the actor's ReferenceForm guid (the child-record convention);
// reference-level changes (position, enabled, count, cell) are plain
// ReferenceForm field PATCHES in the same plugin.
//
// Persistence contract (§6): BaseValues + the ACTIVE durational effects;
// CurrentValues are recomputed on load. Instant effects are already baked
// into the bases — they never appear here.

namespace data {
class FormTypeRegistry;
}

namespace gameplay {

// One per captured actor — its existence IS the "this actor was captured"
// sentinel (spawn applies saved state instead of rolling the loadout).
//
// Its reflected fields are GENERATED from the persisted components'
// TypeInfos (savedStatsComponents(), SaveState.hpp) rather than mirrored
// by hand: every reflected field of every persisted component, minus the
// savedStatsExclusions() table, becomes one field of the same name and
// kind — so a field added to a component is saved without anyone
// remembering to mirror it. Field ids are fnv1a(name) on both sides, which
// is what plugins and saves store: a generated field carries exactly the
// id its hand-written predecessor did, and existing saves resolve
// unchanged. Slot defaults are the components' own C++ defaults, so a
// record keeps carrying only what differs from spawn state.
//
// Hand-written: `parent`, the child-record link to the actor's
// ReferenceForm (not a component mirror). Generated: everything else,
// stored kind-typed in `values`, one slot per distinct field name. Two
// components reflecting the same name share ONE slot (the capture sweep
// writes it from whichever component is swept last, the apply sweep
// reads it into both) and must agree on the kind — the generator
// asserts, the coverage test reports.
//
// Flags and enum tables of the component fields are NOT carried: a save
// field is always persisted (never Transient) and never vocabulary-
// checked by the loader.
struct SavedStatsForm : data::Form {
    core::Guid parent; // the actor's ReferenceForm

    // The generated fields' storage: values[slot], kind fixed per slot by
    // the generated FieldInfo, sized and defaulted by the constructor.
    vector<reflect::Value> values;

    SavedStatsForm();

    // Built once from the persisted components; same type name/id and
    // parent (data::Form) as a REFLECT_BEGIN declaration would produce.
    static const reflect::TypeInfo& staticTypeInfo();

    // Typed by-name access to a field (generated or hand-written). A name
    // no field carries, or a kind the field does not hold, is a
    // programming error: asserts in Debug, yields T{} / writes nothing in
    // Release.
    template<typename T>
    T get(std::string_view name) const {
        const reflect::FieldInfo* field = staticTypeInfo().findField(name);
        ENGINE_ASSERT_MSG(field != nullptr, "SavedStatsForm has no field '{}'",
                          name);
        if (!field) {
            return T {};
        }
        const reflect::Value value = field->get(this);
        const T* typed = std::get_if<T>(&value);
        ENGINE_ASSERT_MSG(typed != nullptr,
                          "SavedStatsForm.{} does not hold the requested kind",
                          name);
        return typed ? *typed : T {};
    }

    template<typename T>
    void set(std::string_view name, const T& value) {
        const reflect::FieldInfo* field = staticTypeInfo().findField(name);
        ENGINE_ASSERT_MSG(field != nullptr, "SavedStatsForm has no field '{}'",
                          name);
        if (!field) {
            return;
        }
        [[maybe_unused]] const bool written =
            field->set(this, reflect::Value { value });
        ENGINE_ASSERT_MSG(written,
                          "SavedStatsForm.{} does not hold the requested kind",
                          name);
    }
};

// One per ACTIVE durational effect. Mirrors gameplay::ActiveEffect
// directly (an ActiveEffect does not know its source EffectForm, and one
// form can spawn several rows) — restored by restoreActiveEffect(), NOT
// by re-applying a form. `attribute` is the raw fnv1a field id (stable
// across runs; effects may target attributes of ANY set, no generic
// reverse name lookup exists); the tag keeps its dotted name.
struct SavedEffectForm : data::Form {
    core::Guid parent;  // the actor's ReferenceForm
    u32 attribute { 0 }; // attr field id (gameplay::attr("health")...)
    i32 op { 0 };       // ModifierOp
    f32 magnitude { 0.0f };
    bool infinite { false };
    f32 remaining { 0.0f };
    f32 period { 0.0f };
    f32 sinceLastTick { 0.0f };
    str grantedTag;     // dotted tag name ("" = none)
    bool decayOnExpiry { false };
    f32 decayPerHour { 1.0f };
    f32 expiryMagnitude { 0.0f };
    bool gameTime { false };
    // The runtime effectId at capture: rows one EffectForm spawned (its
    // attribute2 line) share it, and the restore pairs them again under
    // ONE fresh id so removeEffectById keeps taking both out. Only the
    // pairing survives; 0 = an older save or an untracked row.
    u32 effectId { 0 };

    REFLECT_BEGIN(SavedEffectForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(attribute)
        REFLECT_FIELD(op)
        REFLECT_FIELD(magnitude)
        REFLECT_FIELD(infinite)
        REFLECT_FIELD(remaining)
        REFLECT_FIELD(period)
        REFLECT_FIELD(sinceLastTick)
        REFLECT_FIELD(grantedTag)
        REFLECT_FIELD(decayOnExpiry)
        REFLECT_FIELD(decayPerHour)
        REFLECT_FIELD(expiryMagnitude)
        REFLECT_FIELD(gameTime)
        REFLECT_FIELD(effectId)
    REFLECT_END()
};

// One per inventory stack.
struct SavedItemForm : data::Form {
    core::Guid parent; // the actor's ReferenceForm
    core::Guid item;
    i32 count { 0 };

    REFLECT_BEGIN(SavedItemForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(item)
        REFLECT_FIELD(count)
    REFLECT_END()
};

// One per active injury.
struct SavedInjuryForm : data::Form {
    core::Guid parent; // the actor's ReferenceForm
    i32 type { 0 };    // InjuryType
    i32 part { 0 };    // BodyPart
    i32 severity { 0 };
    f32 recoveryHoursRemaining { 0.0f };

    REFLECT_BEGIN(SavedInjuryForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(type)
        REFLECT_FIELD(part)
        REFLECT_FIELD(severity)
        REFLECT_FIELD(recoveryHoursRemaining)
    REFLECT_END()
};

// One per granted ability — without these rows, everything
// else on the AbilitySystem persists but grantedAbilities would not, and
// a follower's special power and the player's learned perks would vanish
// across a save/load. Pattern B child records like SavedItemForm above;
// applied on load through grantAbility (idempotent — re-applying on top
// of a class perk sync never duplicates).
struct SavedAbilityForm : data::Form {
    core::Guid parent;  // the actor's ReferenceForm
    core::Guid ability; // the AbilityForm guid

    REFLECT_BEGIN(SavedAbilityForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(ability)
    REFLECT_END()
};

// One per trained skill (skills-by-use). Mirrors one
// SkillProgress map entry — Pattern B child records like SavedItemForm.
// Threshold effects are NOT re-applied on load: instant ones are baked
// into the saved BaseValues, durational ones ride as SavedEffectForm rows;
// `granted` restores the applied-prefix count so they never double.
struct SavedSkillForm : data::Form {
    core::Guid parent; // the actor's ReferenceForm
    core::Guid skill;  // the SkillForm guid
    f32 xp { 0.0f };
    i32 granted { 0 };

    REFLECT_BEGIN(SavedSkillForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(skill)
        REFLECT_FIELD(xp)
        REFLECT_FIELD(granted)
    REFLECT_END()
};

// One per faction holding a bounty slice (per-faction crime).
// The total stays SavedStatsForm.bounty (name-matched); rows carry the
// attribution by dotted tag NAME (runtime tag ids aren't stable).
struct SavedBountyForm : data::Form {
    core::Guid parent; // the actor's ReferenceForm
    str faction;       // dotted tag name ("Faction.VillageGuard")
    f32 amount { 0.0f };

    REFLECT_BEGIN(SavedBountyForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(faction)
        REFLECT_FIELD(amount)
    REFLECT_END()
};

// One per save: the world-level state (clock, worldspace, camera).
struct WorldStateForm : data::Form {
    f64 gameSeconds { 0.0 };
    f32 timescale { 12.0f };
    core::Guid activeWorldspace;
    f32 playerYaw { 0.0f };
    f32 playerPitch { 0.0f };
    // The FLY camera — a Spectator/Edit save resumes where the camera
    // stood, not at the player ({0,0,0} = legacy save, not applied).
    Vec3 cameraPosition { 0.0f };
    bool playMode { true };
    // Play/Edit/Spectator needs the tri-state: playMode wins, then
    // editMode, else Spectator.
    bool editMode { false };
    i32 weatherSelected { -1 };
    // The active bounded map (chantier CARTES M4.4): a save on map
    // (1, 0) must reload map (1, 0) — restoring the position without
    // the map put the player in the previous map's ocean.
    bool sandboxMap { false };
    i32 activeMapX { 0 };
    i32 activeMapZ { 0 };

    REFLECT_BEGIN(WorldStateForm, data::Form)
        REFLECT_FIELD(gameSeconds)
        REFLECT_FIELD(timescale)
        REFLECT_FIELD(activeWorldspace)
        REFLECT_FIELD(playerYaw)
        REFLECT_FIELD(playerPitch)
        REFLECT_FIELD(cameraPosition)
        REFLECT_FIELD(playMode)
        REFLECT_FIELD(editMode)
        REFLECT_FIELD(weatherSelected)
        REFLECT_FIELD(sandboxMap)
        REFLECT_FIELD(activeMapX)
        REFLECT_FIELD(activeMapZ)
    REFLECT_END()
};

void registerSaveFormTypes(data::FormTypeRegistry& registry);

} // namespace gameplay
