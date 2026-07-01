#pragma once

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayTags.hpp"

// Drugs are now expressed as EffectForms: channel resonance boost + grantedTag
// "Status.HarmonyBroken" + expiryMode "decay" + expiryMagnitude (aftershock).
// This file retains only the harmony-break query used by the cascade pipeline.

namespace gameplay {

// Whether the harmony cascade is currently broken (a drug is active).
bool harmonyBroken(const AbilitySystem& system, const GameplayTagRegistry& tags);

} // namespace gameplay
