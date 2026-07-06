#pragma once

#include "data/forms/Form.hpp"

// The save-game record types (chantier 5). A SAVE IS AN ORDINARY PLUGIN
// (§2.4/§5 — the non-negotiable): these Forms are its records, resolved by
// the same resolver as every mod, written by the same TomlWriter, cooked
// by the same cooker. Per-actor state rides as CHILD records keyed by
// `parent` = the actor's ReferenceForm guid (the §C.1 convention);
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
// Fields mirror the reflected stat components BY NAME, so capture/apply
// copy them generically through reflection (same fnv1a field ids).
struct SavedStatsForm : data::Form {
    core::Guid parent; // the actor's ReferenceForm

    // CoreAttributes.
    f32 strength { 6.0f };
    f32 constitution { 6.0f };
    f32 grace { 6.0f };
    f32 dexterity { 6.0f };
    f32 alacrity { 6.0f };
    f32 perception { 6.0f };
    f32 charisma { 6.0f };
    f32 ego { 6.0f };
    f32 insight { 0.0f };
    // AttributeSet BaseValues (`damage` is the transient meta-attribute —
    // deliberately absent).
    f32 health { 100.0f };
    f32 maxHealth { 100.0f };
    f32 energy { 100.0f };
    f32 maxEnergy { 100.0f };
    f32 essence { 50.0f };
    f32 maxEssence { 50.0f };
    f32 armorRating { 0.0f };
    // Resonance channels.
    f32 onyx { 0.0f };
    f32 amber { 0.0f };
    f32 garnet { 0.0f };
    // Survival needs.
    f32 hunger { 100.0f };
    f32 thirst { 100.0f };
    f32 sleep { 100.0f };
    // Status buildup channels.
    f32 poison { 0.0f };
    f32 bleed { 0.0f };
    f32 mental { 0.0f };
    f32 disease { 0.0f };
    f32 curse { 0.0f };
    f32 death { 0.0f };
    f32 ignition { 0.0f };
    f32 glaciation { 0.0f };
    f32 electrocution { 0.0f };
    // CombatState.
    f32 posture { 0.0f };
    f32 staggerSeconds { 0.0f };
    f32 paralysisSeconds { 0.0f };
    f32 restSeconds { 0.0f };
    // Equipment slots.
    core::Guid weapon;
    core::Guid head;
    core::Guid torso;
    core::Guid arms;
    core::Guid legs;
    // Chantier 6 APPENDs (ordinals stable): CombatState timers, vendor
    // restock clock, crime bounty (name-mirrored like everything above).
    f32 critWindowSeconds { 0.0f };
    f32 shakenSeconds { 0.0f };
    f32 lastRestockHours { 0.0f };
    f32 bounty { 0.0f };

    REFLECT_BEGIN(SavedStatsForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(strength)
        REFLECT_FIELD(constitution)
        REFLECT_FIELD(grace)
        REFLECT_FIELD(dexterity)
        REFLECT_FIELD(alacrity)
        REFLECT_FIELD(perception)
        REFLECT_FIELD(charisma)
        REFLECT_FIELD(ego)
        REFLECT_FIELD(insight)
        REFLECT_FIELD(health)
        REFLECT_FIELD(maxHealth)
        REFLECT_FIELD(energy)
        REFLECT_FIELD(maxEnergy)
        REFLECT_FIELD(essence)
        REFLECT_FIELD(maxEssence)
        REFLECT_FIELD(armorRating)
        REFLECT_FIELD(onyx)
        REFLECT_FIELD(amber)
        REFLECT_FIELD(garnet)
        REFLECT_FIELD(hunger)
        REFLECT_FIELD(thirst)
        REFLECT_FIELD(sleep)
        REFLECT_FIELD(poison)
        REFLECT_FIELD(bleed)
        REFLECT_FIELD(mental)
        REFLECT_FIELD(disease)
        REFLECT_FIELD(curse)
        REFLECT_FIELD(death)
        REFLECT_FIELD(ignition)
        REFLECT_FIELD(glaciation)
        REFLECT_FIELD(electrocution)
        REFLECT_FIELD(posture)
        REFLECT_FIELD(staggerSeconds)
        REFLECT_FIELD(paralysisSeconds)
        REFLECT_FIELD(restSeconds)
        REFLECT_FIELD(weapon)
        REFLECT_FIELD(head)
        REFLECT_FIELD(torso)
        REFLECT_FIELD(arms)
        REFLECT_FIELD(legs)
        REFLECT_FIELD(critWindowSeconds)
        REFLECT_FIELD(shakenSeconds)
        REFLECT_FIELD(lastRestockHours)
        REFLECT_FIELD(bounty)
    REFLECT_END()
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

// One per save: the world-level state (clock, worldspace, camera).
struct WorldStateForm : data::Form {
    f64 gameSeconds { 0.0 };
    f32 timescale { 12.0f };
    core::Guid activeWorldspace;
    f32 playerYaw { 0.0f };
    f32 playerPitch { 0.0f };
    bool playMode { true };
    i32 weatherSelected { -1 };

    REFLECT_BEGIN(WorldStateForm, data::Form)
        REFLECT_FIELD(gameSeconds)
        REFLECT_FIELD(timescale)
        REFLECT_FIELD(activeWorldspace)
        REFLECT_FIELD(playerYaw)
        REFLECT_FIELD(playerPitch)
        REFLECT_FIELD(playMode)
        REFLECT_FIELD(weatherSelected)
    REFLECT_END()
};

void registerSaveFormTypes(data::FormTypeRegistry& registry);

} // namespace gameplay
