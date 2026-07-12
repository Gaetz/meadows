#pragma once

#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp" // ecs::Entity, ecs::World

namespace data {
class FormDatabase;
}
namespace gameplay {
struct Event;
class GameplayTagRegistry;
struct StatsTuningForm;
}
namespace render {
struct TerrainParams;
}
namespace world {
class CellLoader;
}

namespace game {

class NpcDirector;
struct Npc;
class PendingSaveLayer;

// The scene systems recruit/dismiss touch, bundled so the follower logic is
// decoupled from LandscapeScene (the *Controller pattern — mirrors
// NpcContext / QuestContext). Rebuilt per call (cheap).
struct FollowerContext {
    ecs::World& world;
    data::FormDatabase& forms;
    gameplay::GameplayTagRegistry& gameTags;
    const gameplay::StatsTuningForm& statsTuning;
    const render::TerrainParams& terrainParams;
    world::CellLoader* cellLoader;   // resident check for the dismiss home
    PendingSaveLayer& pendingSave;   // the chantier-5 persistence contract
    ecs::Entity playerEntity;
    NpcDirector& npcDirector;        // the live Npc records (teleport/path)
};

// Recruit/dismiss + the party teleports (FOLLOWERS É1). The persistence is
// the chantier-5 contract, NOT a new mechanism (§2.11): recruiting patches
// the follower's ReferenceForm.cell -> 0 (the persistent set — the player's
// own status) THROUGH the pending save layer (captureEntity diffs the live
// RefId.cell against the resolved record) and drops the ecs::InCell
// relation so his origin cell's unload no longer deletes him; dismissing
// homes him back (cell -> home) the same way. F5/F9 carry both for free —
// a save is the pending layer flushed as an ordinary plugin (§5).
class FollowerController {
public:
    // Dialogue "OnRecruitFollower": flips FollowerState.followerActive,
    // applies the recruit half of the contract, mirrors Follower.Active
    // onto the player (dialogue conditions read PLAYER tags — the
    // Crime.Wanted syncTag pattern).
    void recruit(const FollowerContext& ctx, ecs::Entity follower);

    // Dialogue "OnDismissFollower": reverses it. Home = the ActorForm's
    // homeMarker reference (fallback: the authored spawn). If the home
    // cell is resident the entity re-parents to it and his schedule walks
    // him home; otherwise he is parked at the marker, captured, and
    // despawned — he streams back with his cell. Caller must refreshNpcs
    // after (the despawn path leaves a dead entity in the director list).
    void dismiss(const FollowerContext& ctx, ecs::Entity follower);

    // After performTravel's arrival: any active follower left beyond the
    // teleport radius of `anchor` (the arrival marker) pops in next to it.
    void repositionActiveFollowers(const FollowerContext& ctx,
                                   const Vec3& anchor);

    // Mirror "any active follower" onto the player's Follower.Active tag
    // (registerTag is idempotent). Re-run after spawns/reloads — owned
    // tags are not part of the captured actor state.
    void syncActiveTag(const FollowerContext& ctx);

    // É2 — the aggro table, on the signals combat ALREADY publishes
    // (§2.11: resolveMeleeStrike's OnHitTaken / the director's OnDeath —
    // the R1 strike resolution was target-agnostic from day one). The
    // per-NPC decision is the pure gameplay::adoptOnHit (doctested
    // headless); this handler only resolves the parties' roles and
    // writes Npc.combatTarget (runtime-only — never saved, re-acquired
    // after load from the next landed hit). The Combat.FriendlyTrial
    // tag on the PLAYER suppresses follower adoption (the brawl case).
    void onHitTaken(const FollowerContext& ctx, const gameplay::Event& event);

    // A dead entity is nobody's target: clear every matching
    // combatTarget — followers fall back to the follow package.
    void onDeath(const FollowerContext& ctx, const gameplay::Event& event);

    // The one teleport routine (travel arrivals AND the follow package's
    // too-far case): grounded next to `anchor`, path cleared.
    static void teleportNear(const Vec3& anchor,
                             const render::TerrainParams& terrain, Npc& npc);
};

} // namespace game
