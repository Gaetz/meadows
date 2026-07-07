#pragma once

#include <optional>
#include <unordered_map>

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayTags.hpp"

namespace ecs {
class World;
}

namespace gameplay {

// How a modifier combines with an attribute's value.
enum class ModifierOp { Add, Multiply, Override };

// A modifier currently affecting an AbilitySystem, from a Duration/Infinite or
// Periodic effect. Non-periodic (period == 0) entries contribute to the
// CurrentValue aggregation (a temporary modifier); periodic entries (period > 0)
// instead re-apply to BaseValue on each tick and are excluded from aggregation.
struct ActiveEffect {
    u32 attribute { 0 };
    ModifierOp op { ModifierOp::Add };
    f32 magnitude { 0.0f };
    bool infinite { false };
    f32 remaining { 0.0f };    // seconds left (ignored if infinite); game-seconds if gameTime
    f32 period { 0.0f };       // periodic interval; 0 = not periodic
    f32 sinceLastTick { 0.0f };
    GameplayTag grantedTag {}; // dropped (ref-counted) when the effect ends
    bool decayOnExpiry { false }; // resonance channels only: fade toward 0 on expiry
    f32  decayPerHour  { 1.0f };  // pts/game-hour for the decay phase
    f32  expiryMagnitude { 0.0f }; // initial ResonanceDecay value if != 0 (drug aftershock)
    bool gameTime { false };       // if true, remaining is in game-seconds (tickGameTimeEffects)
    u32  effectId { 0 };           // unique handle for targeted removal (0 = untracked)
};

// The AbilitySystem (the ASC, §6): one per actor (player and NPC alike). Owns
// the runtime gameplay state — owned tags, the current-value overlay, and the
// active effects (granted abilities in 3d). Runtime state only. Persistence
// (save layer, chantier 5): BaseValues live in the reflected AttributeSet; the
// active durational effects are mirrored as SavedEffect records by SaveState
// (captureActiveEffects / restoreActiveEffect; currents re-derived on load, §6).
// Not reflected — its members (containers) are not reflectable; it is a plain
// flecs component.
struct AbilitySystem {
    TagContainer tags;
    std::unordered_map<u32 /*attr field id*/, f32> current; // current-value overlay
    vector<ActiveEffect> activeEffects;
    vector<core::Guid> grantedAbilities; // AbilityForm guids this actor can activate
    u32 nextEffectId { 1 };              // counter for effect ID allocation
    f32 energyRegenDelay { 0.0f };       // seconds before energy regen resumes after
                                         // a spend (set in applyEffect, counted down
                                         // in CharacterTick); 0 = regen active
};

// Registers the GAS components in the ECS: AttributeSet through the reflection
// bridge (its base values serialize), AbilitySystem as a plain runtime
// component. Call once per World at startup.
void registerGameplayComponents(ecs::World& world);

// --- Attribute accessors (addressed by reflection / overlay) ---

// BaseValue of an attribute, read from the set via reflection; nullopt if the
// field is absent or not an f32.
std::optional<f32> baseValueOf(const AttributeSet& set, u32 attrId);
bool setBaseValue(AttributeSet& set, u32 attrId, f32 value);

// Seeds the current-value overlay from the set's base values (current = base for
// every reflected f32 attribute). Call after spawn / on load (§2.9).
void initializeCurrent(AbilitySystem& system, const AttributeSet& set);

f32 currentValueOf(const AbilitySystem& system, u32 attrId); // 0 if absent

} // namespace gameplay
