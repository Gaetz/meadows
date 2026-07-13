#pragma once

#include <unordered_map>

#include "data/forms/Form.hpp"
#include "engine/ecs/World.hpp"

// Skills-by-use v1 (acted decision 2026-07-05; F-catalogue "Progression").
// Every successful ability activation dispatches OnAbilityUsed on the
// EventBus; an ability mapped to a skill (AbilityForm.skill) grants that
// skill's xpPerUse; crossing an authored threshold applies its EffectForm
// to the actor — progression rewards ARE GameplayEffects (§2.9: perks =
// passive effects, the existing model; no new mutation path). Everything
// is data (§5): a mod adds a skill, remaps an ability or retunes
// thresholds purely in TOML.
//
// Persistence: SkillProgress rides as SavedSkillForm child records in the
// save plugin (gameplay/save — the Inventory/Injuries idiom); threshold
// effects need no re-application on load (instant ones are baked into
// BaseValues, durational ones are captured as SavedEffectForm rows).

namespace data {
class FormDatabase;
class FormTypeRegistry;
}

namespace gameplay {

struct AttributeSet;
struct AbilitySystem;
class GameplayTagRegistry;
class EventBus;

struct SkillForm : data::Form {
    str name;              // display name (loc key when the UI shows it)
    f32 xpPerUse { 1.0f }; // XP granted per OnAbilityUsed

    REFLECT_BEGIN(SkillForm, data::Form)
        REFLECT_FIELD(name)
        REFLECT_FIELD(xpPerUse)
    REFLECT_END()
};

// One rank = one child record (the §5 child convention): a mod adds a
// rank by adding a record, never by touching the skill.
struct SkillThresholdForm : data::Form {
    core::Guid parent; // SkillForm
    f32 xp { 10.0f };  // crossed when the skill's total XP reaches this
    core::Guid effect; // EffectForm applied to the actor (perk/passive)

    REFLECT_BEGIN(SkillThresholdForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(xp)
        REFLECT_FIELD(effect)
    REFLECT_END()
};

void registerSkillFormTypes(data::FormTypeRegistry& registry);

// Per-actor progression state. Plain runtime component (variable
// cardinality — not flat-reflectable): persistence is explicit
// SavedSkillForm rows, like Inventory stacks.
struct SkillEntry {
    f32 xp { 0.0f };
    i32 granted { 0 }; // thresholds already applied, in ascending-xp order
};
struct SkillProgress {
    std::unordered_map<core::Guid, SkillEntry> skills;
};

// One use of `skill`: adds xpPerUse, then applies every newly crossed
// threshold's effect in ascending-xp order (deterministic; `granted`
// guarantees a threshold never applies twice). Headless.
void awardSkillUse(const data::FormDatabase& forms, const core::Guid& skill,
                   SkillProgress& progress, AttributeSet& attributes,
                   AbilitySystem& system, const GameplayTagRegistry& tags);

// Wires the loop: OnAbilityUsed(name = ability editorId, source = caster)
// -> AbilityForm.skill -> awardSkillUse on the source entity. The entity
// must ALREADY carry SkillProgress (added at spawn finalize) — the handler
// never adds components (dispatch may run inside a locked iteration).
// Returns the subscription id.
u32 bindSkillProgression(EventBus& bus, const data::FormDatabase& forms,
                         const GameplayTagRegistry& tags);

} // namespace gameplay
