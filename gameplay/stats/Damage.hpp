#pragma once

#include <cstddef>
#include <iterator>
#include <string_view>

#include "data/forms/CoreForms.hpp" // WeaponForm / ArmorForm member pointers (kDamageTypes)
#include "engine/core/Defines.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/DerivedStats.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/ability/Tags.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/StatsTuning.hpp"

// Typed damage + posture/stagger (docs/STATS.md §3-§4). The mitigation pipeline is
// the "custom execution calculation" the GAS deferred (§6): per channel, a flat
// reduction (defense / will, capped) then a percentage reduction (armor /
// resistance), summed onto health. Posture is a combat resource (not a GAS
// attribute); depleting it staggers the actor.

namespace gameplay {

// Physical (slash/pierce/blunt) + elemental. The nine elements map to the three
// essence attributes (docs/STATS.md §3): charisma → fire/sonic/holy,
// ego → cold/chemical/dark, insight → lightning/psychic/ether.
enum class DamageType {
    Slash, Pierce, Blunt,
    Fire, Cold, Lightning,
    Sonic, Chemical, Psychic, Holy, Dark, Ether,
    Count,
};

// The injury family a channel feeds (docs/STATS.md §5): edges cut, blunt
// bruises or fractures, elemental damage injures nothing.
enum class InjuryClass : u8 { None, Edged, Blunt };

// Everything the engine knows about one damage type, in one row. Every
// per-type trait (name, physical/elemental routing, which derived stat
// mitigates it and which primary attribute feeds that stat, the weapon
// channel that deals it, the armor field folded into it, the injury class)
// is read from here — no per-type switch anywhere else.
struct DamageTypeTraits {
    const char* name;            // enum spelling: "Cue.Hit." + name, logs
    bool physical;               // flat mitigation by defense/constitution, else will/ego
    const char* mitigationAttr;  // the derived % stat: "armorSlash" / "resistFire"
    const char* sourceAttr;      // the primary attribute the derived stat scales on
    f32 data::WeaponForm::* attack;    // the weapon channel; nullptr = weapons never deal it
    f32 data::ArmorForm::* mitigation; // the armor field folded into mitigationAttr
    InjuryClass injury;
};

// Physical: armor from strength/constitution/grace. Elemental resistances map
// the three essence attributes (docs/STATS.md §3): charisma → fire/sonic/holy,
// ego → cold/chemical/dark, insight → lightning/psychic/ether.
inline constexpr DamageTypeTraits kDamageTypes[] = {
    { "Slash",     true,  "armorSlash",      "strength",     &data::WeaponForm::slashAttack,     &data::ArmorForm::armorSlash,      InjuryClass::Edged },
    { "Pierce",    true,  "armorPierce",     "grace",        &data::WeaponForm::pierceAttack,    &data::ArmorForm::armorPierce,     InjuryClass::Edged },
    { "Blunt",     true,  "armorBlunt",      "constitution", &data::WeaponForm::bluntAttack,     &data::ArmorForm::armorBlunt,      InjuryClass::Blunt },
    { "Fire",      false, "resistFire",      "charisma",     &data::WeaponForm::fireAttack,      &data::ArmorForm::resistFire,      InjuryClass::None  },
    { "Cold",      false, "resistCold",      "ego",          nullptr,                            &data::ArmorForm::resistCold,      InjuryClass::None  },
    { "Lightning", false, "resistLightning", "insight",      &data::WeaponForm::lightningAttack, &data::ArmorForm::resistLightning, InjuryClass::None  },
    { "Sonic",     false, "resistSonic",     "charisma",     nullptr,                            &data::ArmorForm::resistSonic,     InjuryClass::None  },
    { "Chemical",  false, "resistChemical",  "ego",          nullptr,                            &data::ArmorForm::resistChemical,  InjuryClass::None  },
    { "Psychic",   false, "resistPsychic",   "insight",      nullptr,                            &data::ArmorForm::resistPsychic,   InjuryClass::None  },
    { "Holy",      false, "resistHoly",      "charisma",     nullptr,                            &data::ArmorForm::resistHoly,      InjuryClass::None  },
    { "Dark",      false, "resistDark",      "ego",          nullptr,                            &data::ArmorForm::resistDark,      InjuryClass::None  },
    { "Ether",     false, "resistEther",     "insight",      nullptr,                            &data::ArmorForm::resistEther,     InjuryClass::None  },
};
static_assert(std::size(kDamageTypes) == static_cast<size_t>(DamageType::Count),
              "kDamageTypes must mirror DamageType, in enum order");

constexpr const DamageTypeTraits& damageTraits(DamageType type) {
    return kDamageTypes[static_cast<size_t>(type)];
}

// The type's name, matching the enum spelling — tag composition
// ("Cue.Hit." + name, the cue emission points) and logs.
const char* damageTypeName(DamageType type);

struct StatBlock;

// An outright kill through the NORMAL pipeline (kill-z, future
// scripted executions): a damage event no mitigation survives, so death
// flows exactly like any other (life state, OnDeath, persistence).
void killOutright(StatBlock& target, const GameplayTagRegistry& tags,
                  const DerivedStatRegistry& derived,
                  const StatsTuningForm& tuning);

// Fall damage: blunt damage for a
// landing after `fallHeight` meters. 0 below tuning.fallMinHeight, then
// linear per meter. At/past tuning.fallLethalHeight the caller uses
// killOutright instead — no curve survives that. Pure and headless.
f32 fallDamage(f32 fallHeight, const StatsTuningForm& tuning);

struct DamageChannel {
    DamageType type { DamageType::Slash };
    f32 amount { 0.0f };
};

struct DamageEvent {
    vector<DamageChannel> channels;
    f32 postureAmount { 0.0f };
    // Attacker-side offensive stats, carried on the event
    // (plain runtime struct — no ordinal concern). Pens subtract from the
    // target's POSITIVE mitigation only (they reduce protection, they
    // never amplify a vulnerability); `critical` triggers the critical
    // execution (criticalSensitivity% of the TARGET's maxHealth ×
    // `criticalMultiplier`, bypassing armor — docs/STATS.md §4).
    f32 armorPenetration { 0.0f };
    f32 resistPenetration { 0.0f };
    bool critical { false };
    f32 criticalMultiplier { 1.5f };
    // Weapon status buildup carried to the impact (WeaponForm.buildup*,
    // scaled per docs/STATS.md §3 by the weapon's scaling attribute).
    // Applied by the strike tail through tryAddBuildup — gated by the
    // defender's endurance there. Empty = none.
    str buildupType;
    f32 buildupAmount { 0.0f };
};

// The channel type carrying the most damage (amounts summed per type);
// the first in enum order on a tie, Slash for an empty event. The cue
// emission points name their hit by it.
DamageType dominantDamageType(const DamageEvent& event);

struct DamageResult {
    f32 healthDamage { 0.0f };  // mitigated total dealt to health
    f32 postureDamage { 0.0f }; // dealt to posture
    bool staggered { false };   // posture hit 0 this hit
};

// Combat resource state. `posture` is the poise resource (seeded to
// maxPosture); `staggerSeconds` counts down a stagger; `restSeconds` is the
// in-game time since the last hit ("Rest", §5 — the precondition for
// injury/resonance recovery; a hit resets it). Reflected: the save layer
// captures it by field name like the other stat components.
struct CombatState {
    f32 posture { 0.0f };
    f32 staggerSeconds { 0.0f };   // posture break: brief loss of control
    f32 paralysisSeconds { 0.0f }; // glaciation: frozen in place (distinct from stagger)
    f32 restSeconds { 0.0f };
    // The critical window (docs/STATS.md §4) opened by a
    // posture break — posture SITS at 0 until it elapses, then refills;
    // `shakenSeconds` is the short accuracy debuff from a heavy posture hit.
    f32 critWindowSeconds { 0.0f };
    f32 shakenSeconds { 0.0f };
    // The bleedout window while
    // State.Downed (an active follower at 0 HP goes DOWN, not dead — the
    // updateLifeState routing). Counts down in updateDowned; the caller
    // resolves the timeout (recover with an injury / real death roll).
    f32 downedSeconds { 0.0f };

    REFLECT_BEGIN(CombatState, void)
        REFLECT_FIELD(posture)
        REFLECT_FIELD(staggerSeconds)
        REFLECT_FIELD(paralysisSeconds)
        REFLECT_FIELD(restSeconds)
        REFLECT_FIELD(critWindowSeconds)
        REFLECT_FIELD(shakenSeconds)
        REFLECT_FIELD(downedSeconds)
    REFLECT_END()
};

// The bundle of stat components an actor carries — passed to the execution
// functions. In the ECS these come from separate component storages (the scene
// builds a block per actor).
struct StatBlock {
    CoreAttributes& core;
    AttributeSet& vitals;
    AbilitySystem& system;
    CombatState& combat;
};

// Applies a typed damage event to the target: mitigates each channel, deals the
// total to health, deals posture damage, recomputes, and staggers on a posture
// break (granting State.Staggered). Reads the target's derived defenses from the
// overlay, so the stats must have been recomputed first. Deterministic (no rolls).
DamageResult applyDamage(StatBlock& target, const DamageEvent& event,
                         const GameplayTagRegistry& tags,
                         const DerivedStatRegistry& derived,
                         const StatModifiers* extra = nullptr,
                         const StatsTuningForm& tuning = {});

// The tag-backed combat timers: each pairs a CombatState countdown with the
// State.* tag held while it runs. Granting and ticking go through ONE
// writer each, so timer and tag can never disagree.
enum class TimedState : u8 { Staggered, Paralyzed, CriticalWindow, Shaken, Count };

// What expiry does besides dropping the tag.
enum class OnExpire : u8 { None, RefillPosture };

struct TimedStateTraits {
    f32 CombatState::* timer;
    std::string_view tag;
    OnExpire onExpire;
};

// Posture sits at 0 for the whole critical window and refills on expiry
// (docs/STATS.md §4) — the only timer with a side effect.
inline constexpr TimedStateTraits kTimedStates[] = {
    { &CombatState::staggerSeconds,    tags::Staggered,        OnExpire::None          },
    { &CombatState::paralysisSeconds,  tags::Paralyzed,        OnExpire::None          },
    { &CombatState::critWindowSeconds, tags::CriticalWeakness, OnExpire::RefillPosture },
    { &CombatState::shakenSeconds,     tags::Shaken,           OnExpire::None          },
};
static_assert(std::size(kTimedStates) == static_cast<size_t>(TimedState::Count),
              "kTimedStates must mirror TimedState, in enum order");

// Overwrite = the timer restarts at `seconds`; Extend = max(timer, seconds).
enum class Grant : u8 { Overwrite, Extend };

// The ONE writer of a timed state: sets the timer and grants its tag.
void grantTimedState(CombatState& combat, AbilitySystem& system,
                     const GameplayTagRegistry& tags, TimedState state,
                     f32 seconds, Grant grant);

// Counts down every running timer in enum order; a timer that elapses is
// zeroed, its tag dropped and its OnExpire applied.
void tickTimedStates(CombatState& combat, AbilitySystem& system, f32 dt,
                     const GameplayTagRegistry& tags);

// Counts down an active stagger; drops State.Staggered when it elapses.
void updateStagger(CombatState& combat, AbilitySystem& system, f32 dt,
                   const GameplayTagRegistry& tags);

// Counts down glaciation paralysis; drops State.Paralyzed when it elapses.
void updateParalysis(CombatState& combat, AbilitySystem& system, f32 dt,
                     const GameplayTagRegistry& tags);

// Counts down the critical window; on expiry drops State.CriticalWeakness
// and refills posture to maxPosture (it sat at 0 for the whole window).
void updateCritWindow(CombatState& combat, AbilitySystem& system, f32 dt,
                      const GameplayTagRegistry& tags);

// Counts down the shaken debuff; drops State.Shaken when it elapses.
void updateShaken(CombatState& combat, AbilitySystem& system, f32 dt,
                  const GameplayTagRegistry& tags);

// Counts down the bleedout window while State.Downed is
// held (the updateStagger timer pattern). Unlike the other timers it does
// NOT drop the tag itself: it returns true ONCE when the window elapses
// and the CALLER resolves the outcome (gameplay::resolveBleedout — a
// recovery revives above 0 HP and updateLifeState drops the tag; a real
// death lifts the protection and the same single write point kills).
bool updateDowned(CombatState& combat, AbilitySystem& system, f32 dt,
                  const GameplayTagRegistry& tags);

} // namespace gameplay
