#pragma once

#include "data/forms/CoreForms.hpp"
#include "engine/core/Rng.hpp"
#include "engine/ecs/World.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/actors/CharacterTick.hpp"
#include "gameplay/stats/GameClock.hpp"

namespace game::ui {

// Bench-only state: sample gear and sample effects the panel's test
// buttons apply (the StatsScene owns them). The REAL game equips through
// Inventory/Equipment and applies data EffectForms — these samples exist
// so the bench needs no plugin stack.
struct CharacterStatsDemoState {
    data::WeaponForm&      sampleWeapon;
    data::ArmorForm&       sampleArmor;
    bool&                  armorEquipped;
    gameplay::EffectForm&  sampleDrug;      // drug: grantedTag="Status.HarmonyBroken"
    gameplay::EffectForm&  sampleDisease;   // affliction: attribute="amber"
    gameplay::EffectForm&  samplePsychosis; // affliction: attribute="garnet"
    core::Rng&             rng;
};

// The character-stats inspector (docs/STATS.md): base attributes, vitals/
// posture bars, the resonance breakdown histogram (persistent / cascade /
// effects / survival / injuries), survival needs, derived stats, damage &
// time-skip actions, status buildup, injuries, afflictions and drugs.
// Works on ANY actor entity carrying the full character kit.
//
// `demo` = the bench extras (sample gear/effect buttons); null hides
// them — the in-game inspector (F7 on the player) passes null and keeps
// the read-only views plus the component-only actions.
void drawCharacterStatsPanel(ecs::Entity actor,
                             const gameplay::CharacterTickContext& ctx,
                             gameplay::GameClock& clock,
                             const gameplay::StatModifiers& equipMods,
                             CharacterStatsDemoState* demo = nullptr);

} // namespace game::ui
