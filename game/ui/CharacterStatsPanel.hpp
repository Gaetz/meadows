#pragma once

#include <flecs.h>

#include "data/forms/FormDatabase.hpp"
#include "engine/core/Rng.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/actors/CharacterTick.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/EquipmentStats.hpp"
#include "gameplay/stats/GameClock.hpp"
#include "gameplay/stats/GameTime.hpp"
#include "gameplay/stats/Rest.hpp"

namespace data {
struct WeaponForm;
struct ArmorForm;
} // namespace data

namespace game::ui {

// Demo-phase state: items that will eventually come from the entity's Inventory
// and Equipment components. Grouped in one struct so the panel signature stays
// stable as we replace them one by one in Phase 8.
struct CharacterStatsDemoState {
    data::WeaponForm&      sampleWeapon;
    data::ArmorForm&       sampleArmor;
    bool&                  armorEquipped;
    gameplay::EffectForm&  sampleDrug;      // drug: grantedTag="Status.HarmonyBroken"
    gameplay::EffectForm&  sampleDisease;   // affliction: attribute="amber"
    gameplay::EffectForm&  samplePsychosis; // affliction: attribute="garnet"
    core::Rng&             rng;
};

// Draws the full "Character stats" ImGui panel for any actor entity.
// The panel covers: base attributes, vitals/posture bars, resonance breakdown
// histogram, survival, derived stats, actions (damage, heal, time-advance),
// equipment, status buildup, injuries, afflictions, and drugs.
//
// clock    : the scene-level game clock (display + manual advance buttons)
// equipMods: precomputed equipment modifiers (caller resolves from inventory)
// demo     : transient demo state — will be replaced by component queries in Phase 8
void drawCharacterStatsPanel(flecs::entity player,
                             const gameplay::CharacterTickContext& ctx,
                             gameplay::GameClock& clock,
                             const gameplay::StatModifiers& equipMods,
                             CharacterStatsDemoState& demo);

} // namespace game::ui
