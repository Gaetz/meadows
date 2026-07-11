#pragma once

#include "engine/core/Defines.hpp"

// Chantier P0 B3 — the melee combat move decision, sim-pure. ONE flat
// function turns the situation into ONE move (dev rule: real states are
// enums, decided in one place); NpcDirector only EXECUTES the move.
// Ranges come from the weapon (A6): strike inside attackRange, hold the
// preferred band (reach + 1) by strafing while the attack cools down.

namespace gameplay {

enum class CombatMove : u8 {
    Approach, // close in on the target (or investigate its last spot)
    Strike,   // in range, ready (or mid-swing): stand and deliver
    Strafe,   // in the preferred band, attack cooling: orbit the target
    Flee      // too hurt for this fight: run
};

struct CombatSituation {
    f32 distance { 0.0f };       // to the target (m)
    f32 attackRange { 1.8f };    // A6: weapon reach - margin
    f32 preferredRange { 3.4f }; // A6: weapon reach + 1 — the strafe band
    bool canSee { false };       // B2 vision verdict
    bool swinging { false };     // a swing in flight roots the actor
    f32 cooldownSeconds { 0.0f }; // attack cooldown remaining
    f32 healthFraction { 1.0f }; // current / max health, 0..1
    f32 courage { 0.75f };       // ActorForm — flees below (1 - courage)
};

CombatMove chooseCombatMove(const CombatSituation& situation);

} // namespace gameplay
