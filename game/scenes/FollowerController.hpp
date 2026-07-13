#pragma once

#include <functional>
#include <unordered_map>

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"  // É9: the anti-repeat clock map keys
#include "engine/ecs/World.hpp" // ecs::Entity, ecs::World
#include "gameplay/actors/Followers.hpp" // É9: FollowerStance, CommentClock

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
namespace render {
struct TerrainParams;
}
namespace ui {
class UiSystem;
}
namespace world {
class CellLoader;
struct ReferenceForm; // É8: the grave's runtime-created reference
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
    // É7 addition (appended): the currency — the forge upgrade charges
    // through it (the payFine idiom).
    const data::MiscItemForm* goldForm { nullptr };
    // É8 addition (appended): live-spawn a runtime-created PERSISTENT
    // reference (the grave) through the scene's Spawner — the
    // spawnInitialWorld idiom (parent cell = none). Null in headless use.
    std::function<ecs::Entity(const world::ReferenceForm&)> spawnPersistent;
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

    // ---- É9: group commands, banter, ambient comments ----------------------
    // Dialogue "OnPartyFollow/Stay/Attack/Defend" (« Consignes de
    // groupe... » submenu nodes — zero new UI surface; the doc's radial
    // menu is the stated TODO): ONE stance write point for EVERY active
    // follower. Semantics (v1, stated):
    //   Follow — the default É1 follow package + the full É2 aggro table.
    //   Stay   — he stands where he is (the follow dispatch skips him);
    //            his home SCHEDULE takes over only on a DISMISS — staying
    //            keeps him active at his spot. Sandbox v1 (stated): the
    //            home schedules ARE the town life — a dismissed follower
    //            resumes his scheduled day; contextual sandboxing while
    //            grouped comes later.
    //   Attack — one-shot adoption of the player's CURRENT combat target
    //            (the last hostile the player struck — the É2 signal),
    //            then behaves as Follow.
    //   Defend — the É2 default MINUS rule 4: no adoption on the player's
    //            initiative (AggroRoles.defendOnly); only attackers of
    //            the party engage him.
    void partyCommand(const FollowerContext& ctx,
                      gameplay::FollowerStance stance);

    // É9 ambient comments (docs/FOLLOWERS.md §6.1) — the SAME generic bus
    // channel as onAffinityEvent (the É4 subscribeAll precedent): a
    // matching CommentForm child of an ACTIVE follower's ActorForm toasts
    // « {Name} : {line} », gated by the pure gameplay::decideComment
    // (10-game-hour anti-repeat, oneShot, ordered chaining) — never while
    // the player sneaks. Anti-repeat clocks are RUNTIME-ONLY v1 (stated:
    // they reset on load; oneShot is not persisted either).
    void onAmbientEvent(const FollowerContext& ctx,
                        const gameplay::Event& event);

    // onExit: drop the accrual stamp (the game clock restarts with the
    // next scene enter) and the É9 runtime clocks (anti-repeat is
    // per-session v1 — stated).
    void reset() {
        lastAccrualHours_ = -1.0;
        commentClocks_.clear();
        lastBanterHours_ = -1.0;
        pendingReplyLine_.clear();
        pendingReplySeconds_ = 0.0f;
        playerTarget_ = ecs::Entity {};
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

    // ---- É6: the player learns a perk (réciproque) -------------------------
    // Dialogue "OnLearnPerk" (« Apprends-moi quelque chose ») — the option
    // itself is gated in DATA by ConditionForm children (affinity >= 25 +
    // HasTag Zone.Calme, the quiet-place mirror; a sibling refusal with the
    // negated zone clause carries the "not here" hint — the doc's
    // notification). This handler resolves the PARTNER's TaughtPerkForm
    // children and grants the FIRST unlearned one to the PLAYER through
    // gameplay::grantPerk (§2.9: the effect path is applyEffect; dedup =
    // the effect's grantedTag / the granted-ability list), then toasts.
    void teachPerk(const FollowerContext& ctx, ecs::Entity follower);

    // ---- É7: the forge upgrade of the base kit -----------------------------
    // Dialogue "OnForgeUpgrade" (« Améliorons ton équipement à la forge »)
    // — the option is gated in DATA (HasItem gold >= 50 + HasTag
    // Zone.Forge, the É6 quiet-zone trigger mirror). §2.2: a Form never
    // mutates — the upgrade REPLACES each unremovable weapon that names
    // an `upgradesTo` tier with that next-tier record (both unremovable),
    // and re-equips it if the old one was drawn. The gold moves HERE (the
    // payFine idiom — chosen over the node's takeItem because select()
    // dispatches the event BEFORE onNodeFired's removal, and this handler
    // must be able to refuse — nothing to upgrade — without charging).
    // Mercenaries (É10) will get a free variant: gate their option in
    // data without the HasItem clause and skip the charge here.
    void forgeUpgrade(const FollowerContext& ctx, ecs::Entity follower);

    // ---- É8: mort, tombe, enterrement --------------------------------------
    // V1 scope (stated): of the doc's three burials, (a) on the spot and
    // (c) the bury contact ship; (b) CARRY the corpse and bury at a chosen
    // spot is DEFERRED — it needs a ground-placement mechanic (aim a spot,
    // validate it) that nothing else requires yet. TODO(followers É8b):
    // when a placement mechanic exists, add the corpse-as-item flow
    // (« Dépouille de {} », heavy MiscItemForm) on top of buryOnSpot.
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

    // [F] « Enterrer ici » on a DEAD follower's corpse (the É7 InteractAlt
    // action — free on corpses): creates the grave AT the corpse (§2.11 —
    // the generalized pending-layer materialization + the persistent-pass
    // spawn idiom), moves the corpse's whole inventory into it
    // (transferAllItems), then removes the corpse the picked-up-item way
    // (disableReference + destruct). Returns true when the NPC list needs
    // a refresh (the corpse entity was destructed).
    bool buryOnSpot(const FollowerContext& ctx, ecs::Entity corpse);

    // Dialogue "OnBuryFollower" on a bury contact (É0's ActorForm data):
    // finds the dead follower whose buryContact IS the partner and buries
    // him at his authored buryMarker (fallback: where he lies). No new
    // condition kind (v1): the HANDLER checks and answers with a toast
    // when there is nobody to bury. Resident corpses only (v1 — a corpse
    // always is: dead followers stay in the persistent set or lie in the
    // player's cell ring).
    bool buryByContact(const FollowerContext& ctx, ecs::Entity partner);

private:
    // ---- É5: classes, levels, evolution ----------------------------------
    // The rules are the pure gameplay layer (syncFollowerLevel /
    // applyClassLevelChange / bonusAttribute — doctested headless); this
    // helper only resolves the entities and performs the §2.9-sanctioned
    // level-up BASE writes (curve delta + level attribute + the +1
    // points), logging "É5: Aldric level 2 -> 3 (+1 strength)". The next
    // tickCharacter recomputes the currents with vitals PRESERVED — never
    // initializeActorStats (no mid-game full heal). Two callers: the
    // per-frame sweep (active = true — 1:1 tracking, points granted) and
    // recruit (active = false — the re-meet catch-up: half the gap
    // floored, full for a mainCharacter, no points).
    void applyLevelSync(const FollowerContext& ctx, ecs::Entity follower,
                        const data::ActorForm& actor,
                        gameplay::FollowerState& state, bool active);

    // É9: the banter tick of the per-frame sweep — >= 2 active followers
    // near each other, out of combat, not sneaking, every
    // banterIntervalHours: the first eligible BanterForm (plugin order —
    // deterministic §8) toasts lineA and queues lineB.
    void updateBanter(const FollowerContext& ctx, f32 dt, f64 nowHours);

    // É4: the last game-hour the sweep accrued time-together at (-1 = not
    // stamped yet — the first sweep stamps without accruing, so a scene
    // enter or F9 reload never credits the whole clock).
    f64 lastAccrualHours_ { -1.0 };

    // ---- É9 runtime-only state (v1, stated: none of it persists) ----------
    // Anti-repeat clocks, one per BanterForm/CommentForm guid.
    std::unordered_map<core::Guid, gameplay::CommentClock> commentClocks_;
    // The banter cadence stamp (the VendorState hour idiom; -1 = the
    // first sweep stamps without chatting).
    f64 lastBanterHours_ { -1.0 };
    // The queued banter REPLY (interaction.say shows one line at a time —
    // v1: lineB lands ~3 s after lineA through this small timer).
    str pendingReplyLine_;
    f32 pendingReplySeconds_ { 0.0f };
    // É9 « attaquez ma cible » : the last hostile the PLAYER struck (the
    // É2 OnHitTaken signal), adopted one-shot at command time. Runtime
    // only — cleared on its death and on reset().
    ecs::Entity playerTarget_ {};
};

} // namespace game
