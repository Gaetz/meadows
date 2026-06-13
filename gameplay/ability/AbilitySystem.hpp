#pragma once

#include <optional>
#include <unordered_map>

#include "engine/core/Defines.hpp"
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
    f32 remaining { 0.0f };    // seconds left (ignored if infinite)
    f32 period { 0.0f };       // periodic interval; 0 = not periodic
    f32 sinceLastTick { 0.0f };
    GameplayTag grantedTag {}; // dropped (ref-counted) when the effect ends
};

// The AbilitySystem (the ASC, §6): one per actor (player and NPC alike). Owns
// the runtime gameplay state — owned tags, the current-value overlay, and the
// active effects (granted abilities in 3d). Runtime state only: serialization is
// Phase 5 (BaseValues live in the reflected AttributeSet; the active-effects
// list needs the deferred container story). Not reflected — its members
// (containers) are not reflectable; it is a plain flecs component.
struct AbilitySystem {
    TagContainer tags;
    std::unordered_map<u32 /*attr field id*/, f32> current; // current-value overlay
    vector<ActiveEffect> activeEffects;
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
void setCurrentValue(AbilitySystem& system, u32 attrId, f32 value);

} // namespace gameplay
