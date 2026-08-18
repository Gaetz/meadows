#pragma once

#include <functional>
#include <unordered_map>

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"  // the anti-repeat clock map keys
#include "engine/ecs/World.hpp" // ecs::Entity, ecs::World
#include "gameplay/actors/Followers.hpp" // FollowerStance, CommentClock

namespace core {
class Rng;
}
namespace data {
struct ActorForm;
class FormDatabase;
struct MiscItemForm;
class TextTable;
}
namespace gameplay {
struct Event;
struct FollowerState;
struct GameClock;
class GameplayTagRegistry;
struct StatsTuningForm;
}
namespace phys {
class PhysicsWorld;
}
namespace render {
struct TerrainParams;
}
namespace ui {
class UiSystem;
}
namespace world {
class CellLoader;
struct ReferenceForm; // The grave's runtime-created reference
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
    // Interior grounding for the teleports (the terrain::height
    // interiorMode family): the collision mesh replaces the height field.
    bool interiorMode { false };
    phys::PhysicsWorld* physics { nullptr };
    world::CellLoader* cellLoader;   // resident check for the dismiss home
    PendingSaveLayer& pendingSave;   // the pending-save persistence contract
    ecs::Entity playerEntity;
    NpcDirector& npcDirector;        // the live Npc records (teleport/path)
    gameplay::GameClock& gameClock;  // convalescence stamps (game-hours)
    core::Rng& rng;                  // bleedout/aggravation rolls (§8 seeded)
    const data::TextTable* texts { nullptr }; // toast lines (loc keys)
    std::function<void(str line)> say;        // HUD toast (may be empty)
    // The recruit-preview screen — null in headless use:
    ::ui::UiSystem* ui { nullptr };
    ScreenStack* screenStack { nullptr };
    // The currency — the forge upgrade charges
    // through it (the payFine idiom).
    const data::MiscItemForm* goldForm { nullptr };
    // Live-spawn a runtime-created PERSISTENT
    // reference (the grave) through the scene's Spawner — the
    // spawnInitialWorld idiom (parent cell = none). Null in headless use.
    std::function<ecs::Entity(const world::ReferenceForm&)> spawnPersistent;
};

// Recruit/dismiss + the party teleports (docs/FOLLOWERS.md). The persistence
// is the pending-save contract, NOT a new mechanism (§2.11): recruiting patches
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

    // The aggro table, on the signals combat ALREADY publishes
    // (§2.11: resolveMeleeStrike's OnHitTaken / the director's OnDeath —
    // the strike resolution was target-agnostic from day one). The
    // per-NPC decision is the pure gameplay::adoptOnHit (doctested
    // headless); this handler only resolves the parties' roles and
    // writes Npc.combatTarget (runtime-only — never saved, re-acquired
    // after load from the next landed hit). The Combat.FriendlyTrial
    // tag on the PLAYER suppresses follower adoption (the brawl case).
    void onHitTaken(const FollowerContext& ctx, const gameplay::Event& event);

    // A dead entity is nobody's target: clear every matching
    // combatTarget — followers fall back to the follow package.
    void onDeath(const FollowerContext& ctx, const gameplay::Event& event);
    // The same purge for a DESPAWN (a fugitive through the mine door):
    // every held handle to the entity must drop before its destruct.
    void disengage(const FollowerContext& ctx, u64 gone);

    // The one teleport routine (travel arrivals AND the follow package's
    // too-far case): grounded next to `anchor` (groundAt — collision probe
    // in interiors), path cleared.
    // Shared helpers of the three TUs (core / combat / social).
    static const data::ActorForm* followerActorForm(
        const FollowerContext& ctx, ecs::Entity follower);
    static void toast(const FollowerContext& ctx, str line);
    static str followerDisplayName(const FollowerContext& ctx,
                                   ecs::Entity follower);
    static gameplay::PartyCounts countActiveParty(
        const FollowerContext& ctx);
    static bool playerSneaking(const FollowerContext& ctx);
    // The shared burial core: persistent grave reference + inventory
    // move + corpse removal; true when the corpse entity was destructed.
    static bool buryFollower(const FollowerContext& ctx, ecs::Entity corpse,
                             const Vec3& gravePos);
    static void teleportNear(const Vec3& anchor,
                             const render::TerrainParams& terrain,
                             bool interiorMode, phys::PhysicsWorld* physics,
                             Npc& npc);

    // ---- The per-frame follower sweep -------------------------------
    // Per frame (after the director's update). Mirrors Follower.Protected onto
    // each follower's OWN tags (the syncStateTag idiom — what routes 0 HP
    // to Downed in updateLifeState), accrues time-together affinity for
    // the ACTIVE ones (gameplay::accrueTimeTogether on GameClock deltas —
    // the VendorState hour-stamp idiom), ticks the bleedout clock
    // (gameplay::updateDowned) and resolves timeouts through
    // gameplay::resolveBleedout: real death, or recovery-with-injury —
    // and, past the severity bar, CONVALESCENCE: the dismiss walks him
    // home and followerDownedRecoveryHours stamps his unavailability.
    // Returns true when the NPC list needs a refresh (a dismiss to a
    // non-resident home despawns the entity).
    bool updateFollowers(const FollowerContext& ctx, f32 dt);

    // ---- Affinity + the recruit preview --------------------------------
    // The generic bus handler (ONE subscribeAll on the scene hub — the
    // QuestDirector.handleQuestEvent precedent): for every follower whose
    // ActorForm owns AffinityRuleForm children, a matching event moves his
    // affinity (the pure gameplay::affinityDelta, clamped ±100). Eligible:
    // ACTIVE followers — plus the current dialogue partner, so a chat rule
    // (Maela's OnFollowerChat) can grow affinity BEFORE recruitment.
    void onAffinityEvent(const FollowerContext& ctx,
                         const gameplay::Event& event,
                         ecs::Entity dialoguePartner);

    // Dialogue "OnFollowerPreview" — the
    // recruit-preview screen (the MapController screen idiom: push the
    // "recruit" model, show the UiScreenForm screen): name, class, level,
    // the 9 attributes (currentValueOf on the PARTNER entity), vitals,
    // affinity. Loc'd through the TextTable (ui.recruit.* keys).
    void openRecruitPreview(const FollowerContext& ctx, ecs::Entity follower);

    // ---- Group commands, banter, ambient comments ----------------------
    // Dialogue "OnPartyFollow/Stay/Attack/Defend" (the group-orders
    // submenu nodes — zero new UI surface; the doc's radial
    // menu is the stated TODO): ONE stance write point for EVERY active
    // follower. Semantics (v1):
    //   Follow — the default follow package + the full aggro table.
    //   Stay   — he stands where he is (the follow dispatch skips him);
    //            his home SCHEDULE takes over only on a DISMISS — staying
    //            keeps him active at his spot. Sandbox v1: the
    //            home schedules ARE the town life — a dismissed follower
    //            resumes his scheduled day; contextual sandboxing while
    //            grouped comes later.
    //   Attack — one-shot adoption of the player's CURRENT combat target
    //            (the last hostile the player struck — the aggro signal),
    //            then behaves as Follow.
    //   Defend — the default MINUS rule 4: no adoption on the player's
    //            initiative (AggroRoles.defendOnly); only attackers of
    //            the party engage him.
    void partyCommand(const FollowerContext& ctx,
                      gameplay::FollowerStance stance);

    // Ambient comments (docs/FOLLOWERS.md §6.1) — the SAME generic bus
    // channel as onAffinityEvent (the subscribeAll precedent): a
    // matching CommentForm child of an ACTIVE follower's ActorForm toasts
    // "{Name}: {line}", gated by the pure gameplay::decideComment
    // (10-game-hour anti-repeat, oneShot, ordered chaining) — never while
    // the player sneaks. Anti-repeat clocks are RUNTIME-ONLY v1 (stated:
    // they reset on load; oneShot is not persisted either).
    void onAmbientEvent(const FollowerContext& ctx,
                        const gameplay::Event& event);

    // onExit: drop the accrual stamp (the game clock restarts with the
    // next scene enter) and the runtime clocks (anti-repeat is
    // per-session v1). The warned-contract flags go too —
    // v1: not persisted, a reload may repeat the one warning.
    void reset() {
        lastAccrualHours_ = -1.0;
        commentClocks_.clear();
        lastBanterHours_ = -1.0;
        pendingReplyLine_.clear();
        pendingReplySeconds_ = 0.0f;
        playerTarget_ = ecs::Entity {};
        warnedContracts_.clear();
    }

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

    // ---- The player learns a perk (the reverse direction) --------------
    // Dialogue "OnLearnPerk" — the option
    // itself is gated in DATA by ConditionForm children (affinity >= 25 +
    // HasTag Zone.Calme, the quiet-place mirror; a sibling refusal with the
    // negated zone clause carries the "not here" hint — the doc's
    // notification). This handler resolves the PARTNER's TaughtPerkForm
    // children and grants the FIRST unlearned one to the PLAYER through
    // gameplay::grantPerk (§2.9: the effect path is applyEffect; dedup =
    // the effect's grantedTag / the granted-ability list), then toasts.
    void teachPerk(const FollowerContext& ctx, ecs::Entity follower);

    // ---- The forge upgrade of the base kit -----------------------------
    // Dialogue "OnForgeUpgrade" — the option is gated in DATA (HasItem gold >= 50 + HasTag
    // Zone.Forge, the quiet-zone trigger mirror). §2.2: a Form never
    // mutates — the upgrade REPLACES each unremovable weapon that names
    // an `upgradesTo` tier with that next-tier record (both unremovable),
    // and re-equips it if the old one was drawn. The gold moves HERE (the
    // payFine idiom — chosen over the node's takeItem because select()
    // dispatches the event BEFORE onNodeFired's removal, and this handler
    // must be able to refuse — nothing to upgrade — without charging).
    // Mercenaries will get a free variant: gate their option in
    // data without the HasItem clause and skip the charge here.
    void forgeUpgrade(const FollowerContext& ctx, ecs::Entity follower);

    // ---- Mercenaries ---------------------------------------------------
    // Dialogue "OnHireMercenary" (hire / extend-contract — BOTH
    // options fire the same event; the
    // renewal option simply also exists, gated Follower.Active, on the
    // mercenary's own dialogue: `mercenary` is per-actor DATA the
    // condition evaluator cannot read, so authoring carries that half).
    // DECISION: the hire option is gated in data by a COARSE
    // `HasItem gold >= contractBasePrice` — the REAL price is dynamic
    // (gameplay::mercenaryPrice on the player's level and wealth), so the
    // HANDLER re-checks it and refuses with a toast QUOTING the price
    // when the coarse gate passed but the scaled price doesn't — that
    // refusal toast doubles as the price display. The charge is the
    // payFine idiom (handler-side removeItem — a refusal, or a recruit
    // the caps/convalescence bounce, stays FREE); the join is the
    // recruit path unchanged; the expiry stamp is
    // followerContractExpiryHours (GameClock game-hours, the VendorState
    // idiom) written through gameplay::extendContract.
    void hireMercenary(const FollowerContext& ctx, ecs::Entity follower);

    // ---- Mort, tombe, enterrement --------------------------------------
    // V1 scope: of the doc's three burials, (a) on the spot and
    // (c) the bury contact ship; (b) CARRY the corpse and bury at a chosen
    // spot is DEFERRED — it needs a ground-placement mechanic (aim a spot,
    // validate it) that nothing else requires yet. TODO(followers):
    // when a placement mechanic exists, add the corpse-as-item flow
    // (a "{} remains" heavy MiscItemForm) on top of buryOnSpot.
    //
    // The grave reference's DETERMINISTIC guid: Guid::combine of the dead
    // follower's reference guid and the grave namespace (the prefab-child
    // derivation idiom) — burying the same follower twice re-targets the
    // same record, and the guid is recomputable forever (graveOwnerName).
    static core::Guid graveGuidFor(const core::Guid& followerReference);

    // The buried follower's display name, recovered FROM the grave guid:
    // scan the resolved references and match combine(ref, ns) — zero extra
    // persisted state, works across any number of reloads. Empty when the
    // grave's owner is unresolvable (a modded/foreign grave).
    static str graveOwnerName(const data::FormDatabase& forms,
                              const core::Guid& graveReference);

    // [F] "Bury here" on a DEAD follower's corpse (the InteractAlt
    // action — free on corpses): creates the grave AT the corpse (§2.11 —
    // the generalized pending-layer materialization + the persistent-pass
    // spawn idiom), moves the corpse's whole inventory into it
    // (transferAllItems), then removes the corpse the picked-up-item way
    // (disableReference + destruct). Returns true when the NPC list needs
    // a refresh (the corpse entity was destructed).
    bool buryOnSpot(const FollowerContext& ctx, ecs::Entity corpse);

    // Dialogue "OnBuryFollower" on a bury contact (the ActorForm data):
    // finds the dead follower whose buryContact IS the partner and buries
    // him at his authored buryMarker (fallback: where he lies). No new
    // condition kind (v1): the HANDLER checks and answers with a toast
    // when there is nobody to bury. Resident corpses only (v1 — a corpse
    // always is: dead followers stay in the persistent set or lie in the
    // player's cell ring).
    bool buryByContact(const FollowerContext& ctx, ecs::Entity partner);

private:
    // ---- Classes, levels, evolution ----------------------------------
    // The rules are the pure gameplay layer (syncFollowerLevel /
    // applyClassLevelChange / bonusAttribute — doctested headless); this
    // helper only resolves the entities and performs the §2.9-sanctioned
    // level-up BASE writes (curve delta + level attribute + the +1
    // points), logging "Aldric level 2 -> 3 (+1 strength)". The next
    // tickCharacter recomputes the currents with vitals PRESERVED — never
    // initializeActorStats (no mid-game full heal). Two callers: the
    // per-frame sweep (active = true — 1:1 tracking, points granted) and
    // recruit (active = false — the re-meet catch-up: half the gap
    // floored, full for a mainCharacter, no points).
    void applyLevelSync(const FollowerContext& ctx, ecs::Entity follower,
                        const data::ActorForm& actor,
                        gameplay::FollowerState& state, bool active);

    // The banter tick of the per-frame sweep — >= 2 active followers
    // near each other, out of combat, not sneaking, every
    // banterIntervalHours: the first eligible BanterForm (plugin order —
    // deterministic §8) toasts lineA and queues lineB.
    void updateBanter(const FollowerContext& ctx, f32 dt, f64 nowHours);

    // The last game-hour the sweep accrued time-together at (-1 = not
    // stamped yet — the first sweep stamps without accruing, so a scene
    // enter or F9 reload never credits the whole clock).
    f64 lastAccrualHours_ { -1.0 };

    // ---- runtime-only state (v1: none of it persists) ----------
    // Anti-repeat clocks, one per BanterForm/CommentForm guid.
    std::unordered_map<core::Guid, gameplay::CommentClock> commentClocks_;
    // The banter cadence stamp (the VendorState hour idiom; -1 = the
    // first sweep stamps without chatting).
    f64 lastBanterHours_ { -1.0 };
    // The queued banter REPLY (interaction.say shows one line at a time —
    // v1: lineB lands ~3 s after lineA through this small timer).
    str pendingReplyLine_;
    f32 pendingReplySeconds_ { 0.0f };
    // "attack my target": the last hostile the PLAYER struck (the
    // OnHitTaken signal), adopted one-shot at command time. Runtime
    // only — cleared on its death and on reset().
    ecs::Entity playerTarget_ {};
    // Which mercenaries already got their ONE near-expiry warning,
    // keyed by ActorForm guid (stable across despawns). Runtime-only v1
    // (a reload may re-warn once); cleared on renewal — a fresh
    // contract earns a fresh warning.
    std::unordered_map<core::Guid, bool> warnedContracts_;
};

} // namespace game
