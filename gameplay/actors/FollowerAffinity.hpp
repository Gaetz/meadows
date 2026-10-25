#pragma once

#include "engine/core/Defines.hpp"
#include "gameplay/ability/GameplayTags.hpp" // GameplayTag (affinity rules)

namespace gameplay {

struct FollowerState;

// ---- Affinity ----------------------------------------------------------
// Affinity lives on FollowerState (a plain reflected field — §2.9: it is
// NOT a GAS attribute and never moves through applyEffect). Two data-driven
// movers, both pure and doctested: passive growth per game-hour spent
// together (the VendorState hour-stamp idiom, delta computed by the game
// side from GameClock), and AffinityRuleForm children of the ActorForm
// reacting to bus events (the QuestTaskForm event+filterTag matching).

struct AffinityRuleForm;

inline constexpr f32 kAffinityMin { -100.0f };
inline constexpr f32 kAffinityMax { 100.0f };

// Moves `state.followerAffinity` by `delta`, clamped to ±100. Returns the
// APPLIED change (0 at a saturated bound).
f32 addAffinity(FollowerState& state, f32 delta);

// Passive accrual: `deltaHours` game-hours of traveling together land on
// followerHoursTogether and grow affinity by deltaHours × affinityPerHour
// (clamped). Non-positive deltas are no-ops (clock hiccups stay inert).
// Returns the applied affinity change.
f32 accrueTimeTogether(FollowerState& state, f32 deltaHours,
                       f32 affinityPerHour);

// The event side of an AffinityRuleForm match, resolved by the caller (the
// pure matcher never touches entities): the event's kind/tag plus who the
// parties are RELATIVE to the follower being evaluated.
struct AffinityEventView {
    u32 kind { 0 };       // gameplay::eventKind of the event name
    GameplayTag tag {};   // the event's categorizing tag (may be invalid)
    bool sourceIsPlayer { false };
    bool targetIsSelf { false };
};

// Sums the deltas of every rule matching the event: name equality, then
// filterTag (the event's tag must DESCEND from it — tags.isA, the
// QuestTaskForm matching) and the party filters. Pure; the caller applies
// the sum through addAffinity.
f32 affinityDelta(const vector<const AffinityRuleForm*>& rules,
                  const AffinityEventView& event,
                  const GameplayTagRegistry& tags);

} // namespace gameplay
