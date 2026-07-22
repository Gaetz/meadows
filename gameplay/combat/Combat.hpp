#pragma once

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/ability/GameplayTags.hpp"

// Combat (§4, §6): expressed entirely through abilities + effects (damage is an
// effect on Health). This layer adds only the derived "death" state and a thin
// attack helper. Requires the `State.Dead` tag to be registered.

namespace gameplay {

// Derived life-state change hook: grants `State.Dead` when current health has
// reached 0, clears it when the actor is alive again. Idempotent.
// Protected followers (docs/FOLLOWERS.md): an actor holding
// `Follower.Protected` (the mirror of FollowerState.followerActive) gets
// `State.Downed` at 0 HP instead — out of the fight, revivable — and only
// the bleedout resolution (gameplay::resolveBleedout) turns that into a
// real death. Without those tags registered, 0 HP is a plain death.
void updateLifeState(AbilitySystem& system, const GameplayTagRegistry& registry);

// An attack = activate an ability from caster onto target, then refresh the
// target's life state. Returns whether the ability activated.
bool performAttack(const AbilityForm& ability,
                   AttributeSet& casterSet, AbilitySystem& casterSystem,
                   AttributeSet& targetSet, AbilitySystem& targetSystem,
                   const AbilityContext& ctx);

} // namespace gameplay
