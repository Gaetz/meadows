#pragma once

#include <functional>

#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp" // ecs::Entity, ecs::World

namespace core {
class Rng;
}
namespace data {
class FormDatabase;
class TextTable;
}
namespace gameplay {
struct Event;
struct GameClock;
class GameplayTagRegistry;
struct StatsTuningForm;
}
namespace render {
struct TerrainParams;
}
namespace ui {
class UiSystem;
}
namespace world {
class CellLoader;
}

namespace game {

class NpcDirector;
struct Npc;
class PendingSaveLayer;
class ScreenStack;

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
    // É3 additions:
    gameplay::GameClock& gameClock;  // convalescence stamps (game-hours)
    core::Rng& rng;                  // bleedout/aggravation rolls (§8 seeded)
    const data::TextTable* texts { nullptr }; // toast lines (loc keys)
    std::function<void(str line)> say;        // HUD toast (may be empty)
    // É4 additions (the recruit-preview screen — null in headless use):
    ::ui::UiSystem* ui { nullptr };
    ScreenStack* screenStack { nullptr };
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

    // ---- É3/É4: the per-frame follower sweep -------------------------------
    // Per frame (after the director's update) — renamed from updateDowned
    // when É4 added the affinity accrual. Mirrors Follower.Protected onto
    // each follower's OWN tags (the syncStateTag idiom — what routes 0 HP
    // to Downed in updateLifeState), accrues time-together affinity for
    // the ACTIVE ones (gameplay::accrueTimeTogether on GameClock deltas —
    // the VendorState hour-stamp idiom), ticks the bleedout clock
    // (gameplay::updateDowned) and resolves timeouts through
    // gameplay::resolveBleedout: real death, or recovery-with-injury —
    // and, past the severity bar, CONVALESCENCE: the É1 dismiss walks him
    // home and followerDownedRecoveryHours stamps his unavailability.
    // Returns true when the NPC list needs a refresh (a dismiss to a
    // non-resident home despawns the entity).
    bool updateFollowers(const FollowerContext& ctx, f32 dt);

    // ---- É4: affinity + the recruit preview --------------------------------
    // The generic bus handler (ONE subscribeAll on the scene hub — the
    // QuestDirector.handleQuestEvent precedent): for every follower whose
    // ActorForm owns AffinityRuleForm children, a matching event moves his
    // affinity (the pure gameplay::affinityDelta, clamped ±100). Eligible:
    // ACTIVE followers — plus the current dialogue partner, so a chat rule
    // (Maela's OnFollowerChat) can grow affinity BEFORE recruitment.
    void onAffinityEvent(const FollowerContext& ctx,
                         const gameplay::Event& event,
                         ecs::Entity dialoguePartner);

    // Dialogue "OnFollowerPreview" (« Parle-moi de tes aptitudes ») — the
    // recruit-preview screen (the MapController screen idiom: push the
    // "recruit" model, show the UiScreenForm screen): name, class, level,
    // the 9 attributes (currentValueOf on the PARTNER entity), vitals,
    // affinity. Loc'd through the TextTable (C9.5 keys, ui.recruit.*).
    void openRecruitPreview(const FollowerContext& ctx, ecs::Entity follower);

    // onExit: drop the accrual stamp (the game clock restarts with the
    // next scene enter).
    void reset() { lastAccrualHours_ = -1.0; }

    // [E] on a downed ally: the useConsumable path re-aimed at HIM — the
    // first health-restoring ConsumableForm in the PLAYER's bag is
    // consumed and its own EffectForm applies to the follower's stats
    // (§2.9: healing through applyEffect); the life-state derive then
    // drops State.Downed and he stands back up. No potion = a toast.
    void reviveDownedAlly(const FollowerContext& ctx, ecs::Entity follower);

    // Consultation dialogue ("Comment te sens-tu ?" -> OnFollowerStatus):
    // v1 = a HUD toast (TextTable N-args) with health %, injury count and
    // remaining recovery hours — no new screen.
    void consultFollower(const FollowerContext& ctx, ecs::Entity follower);

    // Mirror "any follower convalescent" onto the player's
    // Follower.Convalescent tag (the Follower.Active precedent) so the
    // recruit dialogue's refusal option gates on it.
    void syncConvalescentTag(const FollowerContext& ctx);

private:
    // É4: the last game-hour the sweep accrued time-together at (-1 = not
    // stamped yet — the first sweep stamps without accruing, so a scene
    // enter or F9 reload never credits the whole clock).
    f64 lastAccrualHours_ { -1.0 };
};

} // namespace game
