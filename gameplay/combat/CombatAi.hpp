#pragma once

#include <iterator>
#include <optional>
#include <string_view>

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"

// The melee combat move decision, sim-pure. ONE flat
// function turns the situation into ONE move (real states are
// enums, decided in one place); NpcDirector only EXECUTES the move.
// Ranges come from the weapon: strike inside attackRange, hold the
// preferred band (reach + 1) by strafing while the attack cools down.

namespace gameplay {

enum class CombatMove : u8 {
    Approach, // close in on the target (or investigate its last spot)
    Strike,   // in range, ready (or mid-swing): stand and deliver
    Strafe,   // in the preferred band, attack cooling: orbit the target
    Flee      // too hurt for this fight: run
};
inline constexpr std::string_view kCombatMoveNames[] = { "approach", "strike",
                                                         "strafe", "flee" };
inline constexpr reflect::EnumTable kCombatMoves { kCombatMoveNames };
static_assert(std::size(kCombatMoveNames) ==
              static_cast<size_t>(CombatMove::Flee) + 1);

// A follower class's combat style (FollowerClassForm.combatStyle): how
// the actor uses his special power and holds his band in combat. The
// ranged behaviour itself comes from the weapon (projectileSpeed > 0),
// so Archer is a vocabulary entry with no brain branch of its own.
// kCombatStyles names them in this order; "" = Default.
enum class CombatStyle : u8 {
    Default, // self-cast the power when engaging; the melee brain
    Melee,   // same brain as Default, named for the class vocabulary
    Healer,  // the power goes to the lowest ally; holds a support band
    Archer,  // vocabulary only — the bow decides the behaviour
    Count
};
inline constexpr std::string_view kCombatStyleNames[] = { "default", "melee",
                                                          "healer", "archer" };
inline constexpr reflect::EnumTable kCombatStyles { kCombatStyleNames, true };
static_assert(std::size(kCombatStyleNames) ==
              static_cast<size_t>(CombatStyle::Count));

// "" and any name outside kCombatStyles read as Default (the loader
// already rejects unknown values; this covers hand-built forms).
CombatStyle parseCombatStyle(std::string_view name);

struct CombatSituation {
    f32 distance { 0.0f };       // to the target (m)
    f32 attackRange { 1.8f };    // weapon reach - margin
    f32 preferredRange { 3.4f }; // weapon reach + 1 — the strafe band
    bool canSee { false };       // Perception vision verdict
    bool swinging { false };     // a swing in flight roots the actor
    f32 cooldownSeconds { 0.0f }; // attack cooldown remaining
    f32 healthFraction { 1.0f }; // current / max health, 0..1
    f32 courage { 0.75f };       // ActorForm — flees below (1 - courage)
};

CombatMove chooseCombatMove(const CombatSituation& situation);

// The brain-script contract (docs/BOSS-SCRIPTING.md): a Lua decide()
// returns a move NAME — this maps it back, nullopt for anything else
// (nil, a typo, an error) so the caller falls back to chooseCombatMove.
std::optional<CombatMove> parseCombatMove(std::string_view name);

// The move's name, parseCombatMove's vocabulary backward — the combat
// intent log/view ("why is this NPC running?" — a bow-band strafe
// reads as a flee without it).
const char* combatMoveName(CombatMove move);

} // namespace gameplay
