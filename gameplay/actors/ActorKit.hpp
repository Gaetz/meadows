#pragma once

#include <concepts>

#include "engine/ecs/World.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/actors/ActorState.hpp"
#include "gameplay/combat/MeleeSwing.hpp"
#include "gameplay/inventory/Inventory.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/Injuries.hpp"
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/ResonanceDecays.hpp"
#include "gameplay/stats/Skills.hpp"
#include "gameplay/stats/StatusBuildup.hpp"
#include "gameplay/stats/Survival.hpp"

// The actor component kit: THE list of the components every actor entity
// carries. §2.7 fixes actor composition as a C++ bundle (data-driven
// richness comes from the GAS and scripts, not from data-declared
// components), and one list is what keeps three things from drifting
// apart: what the Spawner wires, what the scene must see present before
// it applies saved state (the name-matched SavedStatsForm fields only land
// on a component that exists), and what the save persists —
// savedStatsComponents() (SaveState.hpp) is the reflected subset of this
// very list, so the generated SavedStatsForm cannot name a component the
// kit does not attach, nor the kit grow a reflected component the save
// forgets.
//
// Kit ORDER is persistence order: the reflected members, in this order,
// are the generated SavedStatsForm's field order and the order saved
// fields are written back on load. flecs itself does not care in which
// order components are attached (an archetype is a set).
//
// attachActorKit never overwrites: a component the entity already has
// keeps its values (the Spawner seeds AttributeSet / AbilitySystem before
// attaching the rest; a bench may pre-fill anything). Hence `has<T>()` for
// a kit type is true on every actor after spawn — readers that gate on it
// are guarding against non-actor entities (a grave holding an Inventory,
// a container), not against partially built actors.
//
// Attaching also links the kit's reflected types into the ECS reflection
// bridge (registerActorKit), the bridge the save layer walks.

namespace gameplay {

template<typename... Ts>
struct TypeList {};

template<typename T>
struct TypeTag {
    using type = T;
};

using ActorKit = TypeList<
    // The character-stats sheet and its GAS runtime (tickCharacter reads all
    // of these on every actor).
    CoreAttributes, AttributeSet, AbilitySystem, Resonance, Survival,
    StatusBuildup, CombatState, Injuries, ResonanceDecays,
    // The per-actor runtime state the scene and the save need present on
    // every actor, player and NPC alike.
    MeleeSwing, Inventory, Equipment, VendorState, Bounty, FollowerState,
    SkillProgress>;

namespace detail {

template<typename Fn, typename... Ts>
void visitTypes(TypeList<Ts...>, Fn& fn) {
    (fn(TypeTag<Ts> {}), ...);
}

} // namespace detail

// Visits every kit type in kit order: fn(TypeTag<T> {}) for each T. Read
// the type back with `using T = typename decltype(tag)::type;`.
template<typename Fn>
void forEachActorKitType(Fn&& fn) {
    detail::visitTypes(ActorKit {}, fn);
}

// A kit component our reflection describes — the persisted subset
// (containers like Inventory or Injuries are kit members without
// reflection; they persist as child records of their own).
template<typename T>
concept ReflectedKitComponent = requires {
    { T::staticTypeInfo() } -> std::convertible_to<const reflect::TypeInfo&>;
};

// Registers every kit component in flecs and links the reflected ones into
// the ECS reflection bridge (ecs::registerReflectedComponent). Idempotent.
// The flecs-handle overload serves callers that hold only an entity
// (entity.world()) — the save path uses it so an actor assembled by hand
// persists through the same bridge as a spawned one.
void registerActorKit(ecs::World& world);
void registerActorKit(const flecs::world& world);

// Attaches every kit component the entity does NOT already carry, default
// constructed; components already present keep their values. Also
// registers the kit (see above).
void attachActorKit(ecs::World& world, ecs::Entity entity);

} // namespace gameplay
