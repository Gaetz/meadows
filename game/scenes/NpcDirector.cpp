#include "game/scenes/NpcDirector.hpp"

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "data/forms/CoreForms.hpp"       // data::WeaponForm
#include "data/forms/FormDatabase.hpp"
#include "engine/core/Log.hpp"            // the downed-edge trace
#include "game/SceneSubmit.hpp"           // RenderSnapshot extract
#include "game/WeaponMeshes.hpp"          // the visible sword guid
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/actors/ActorState.hpp" // FollowerState (follow dispatch)
#include "gameplay/ai/ScheduleSystem.hpp" // updateInterruption edges
#include "gameplay/actors/CharacterTick.hpp"
#include "gameplay/actors/Followers.hpp"  // foldAgeModifiers
#include "gameplay/cue/GameplayCues.hpp"        // Cue.* emissions (C2)
#include "gameplay/event/EventBus.hpp"
#include "gameplay/inventory/Inventory.hpp" // Equipment (the weapon link)
#include "gameplay/stats/EquipmentStats.hpp" // armor protects NPCs too
#include "gameplay/stats/Damage.hpp"    // CombatState (bleedout clock)
#include "gameplay/stats/GameClock.hpp"
#include "world/ai/Perception.hpp"      // hearing (onNoise)
#include "world/scene/Components.hpp"

namespace game {

// The stat-space -> world mapping and the NPC gait now come from
// StatsTuningForm (§5 moddable) — the same scale the player uses, no more
// hand-mirrored copy.

// [cpp-tuning] — the sword grip correction for the UAL hand_r
// joint. Hand-local: fingers run along +Y,
// the thumb sits on +Z, X pierces the palm. Identity put the blade in
// the FOREARM'S prolongation (along the fingers); +90 degrees about X —
// "the axis through the hand" — stands it up out of the fist on the
// thumb side, where a gripped handle actually exits. Applied to the
// DRAWN sword (extract) and the HIT segment (the combat controller's
// updateSwing) alike: the blade that hits stays the blade you see.
const Mat4 kSwordGrip = glm::rotate(
    Mat4 { 1.0f }, glm::radians(90.0f), Vec3 { 1.0f, 0.0f, 0.0f });

// [cpp-tuning] — the raised-guard grip (the hand turns
// a little INWARD so the blade lies oblique across the front). An extra
// roll about the fist axis on top of kSwordGrip; drawn while
// npc.blocking (the hit test never runs during a guard).
const Mat4 kSwordGuardGrip =
    kSwordGrip * glm::rotate(Mat4 { 1.0f }, glm::radians(-40.0f),
                             Vec3 { 0.0f, 0.0f, 1.0f });

void NpcDirector::refreshNpcs(
    rhi::Device& device, const NpcContext& ctx,
    const std::function<void(ecs::Entity, const core::Guid&)>&
        finalizeActorSpawn) {
    spawner_.refreshNpcs(device, ctx, finalizeActorSpawn, npcs_,
                         npcByEntity_, patrolPoints, characterSpot_);
}

Npc* NpcDirector::findNpc(u64 entityId) const {
    const auto it = npcByEntity_.find(entityId);
    return it != npcByEntity_.end() ? it->second : nullptr;
}

void NpcDirector::update(f32 dt, const NpcContext& ctx) {
    const f32 hourOfDay =
        static_cast<f32>(std::fmod(ctx.gameClock.gameHours(), 24.0));
    const f64 gameDt =
        static_cast<f64>(dt) * static_cast<f64>(ctx.gameClock.timescale);
    const gameplay::CharacterTickContext tickCtx { ctx.derivedStats,
                                                   ctx.gameTags,
                                                   ctx.statsTuning };
    const auto deadTag = ctx.gameTags.find("State.Dead");
    const auto downedTag = ctx.gameTags.find("State.Downed");
    // Sneak: a crouched player is HALF the target — sight range, the
    // LOS aim point and the blade capsule all read this one bool.
    bool playerSneaking = false;
    if (ctx.playerEntity.is_alive()) {
        if (const auto sneakTag = ctx.gameTags.find("State.Sneaking")) {
            playerSneaking =
                ctx.playerEntity.get<gameplay::AbilitySystem>().tags.has(
                    *sneakTag);
        }
    }
    for (auto& npcPtr : npcs_) {
        Npc& npc = *npcPtr;
        auto& transform = npc.entity.get_mut<world::Transform>();
        f32 idleDecay = 10.0f;

        // NPCs run the full character pipeline too (effects, stagger, life
        // state) — that's where State.Dead comes from.
        // Age applies its two < 1 multipliers through the
        // SAME StatModifiers channel the player's equipment uses (§2.9:
        // mods rebuilt from data each tick, nothing persisted, no
        // synthetic effects). Ageless actors keep the empty default.
        // Equipped ARMOR folds in too (the player's
        // LandscapeScene equipMods fold, mirrored) — armor now protects
        // NPCs. Iso note: today's loadouts hand out weapons only, so no
        // existing NPC's numbers move until armor is actually given
        // (applyEquipmentModifiers resolves the four armor slots; the
        // weapon contributes through the damage path, not here).
        gameplay::StatModifiers npcMods;
        if (npc.entity.has<gameplay::Equipment>()) {
            gameplay::applyEquipmentModifiers(
                npc.entity.get<gameplay::Equipment>(), ctx.forms, npcMods);
        }
        if (npc.age > 0.0f) {
            gameplay::foldAgeModifiers(npc.age, ctx.statsTuning, npcMods);
        }
        gameplay::tickCharacter(npc.entity, dt, gameDt, tickCtx, npcMods);
        const auto& npcSys = npc.entity.get<gameplay::AbilitySystem>();
        const bool wasDead = npc.dead;
        npc.dead = deadTag && npcSys.tags.has(*deadTag);
        // The live->dead EDGE is the gameplay event — quests
        // (kill tasks) and crime listen on the bus. Reload paths never fire
        // it: refreshNpcs seeds npc.dead from the tag.
        if (npc.dead && !wasDead) {
            ctx.eventBus.dispatch({ gameplay::eventKind("OnDeath"),
                                    ecs::Entity {}, npc.entity,
                                    npc.factionTag });
            if (ctx.cues) { // C2: the fall's LOOK (dust puff by default)
                ctx.cues->emit({ "Cue.Death",
                                 transform.position +
                                     Vec3 { 0.0f, 0.6f, 0.0f },
                                 0.0f });
            }
        }
        // (The corpse is lootable — its Inventory was rolled from the
        // LoadoutEntryForm children at build.)
        if (npc.dead) {
            // The death transition (anim graph, State.Dead gate) plays; the
            // body stays. Despawn: a later slice.
            schedule_.releaseFurniture(ctx, npc); // D1: a corpse frees its seat
            npc.attacking = false;   // a death mid-swing cancels it
            npc.weaponDrawn = false; // the club drops with him (visually)
            npc.path.clear();
            npc.speed = 0.0f;
            npc.anim->setParam("speed", 0.0f);
            npc.anim->update(dt, 0.0f);
            anim::bindPose(npc.rig->skeleton, npc.pose);
            npc.anim->evaluate(npc.pose);
            anim::skinMatrices(npc.rig->skeleton, npc.pose, npc.palette);
            npc.pendingAnimEvents.clear(); // dead men fire no events
            continue;
        }

        // A DOWNED actor (an active follower at 0 HP — the
        // Follower.Protected routing in updateLifeState) is out of the
        // fight but not dead. The downed EDGE (the dead-edge idiom above)
        // seeds the bleedout clock and announces it on the bus; the
        // resolution (revive / recover-with-injury / real death) lives in
        // FollowerController::updateDowned. Visual v1: the SIT anim gate
        // stands in for a kneel (the villager graph has no kneel clip).
        const bool wasDowned = npc.downed;
        npc.downed = downedTag && npcSys.tags.has(*downedTag);
        if (npc.downed && !wasDowned) {
            npc.entity.get_mut<gameplay::CombatState>().downedSeconds =
                ctx.statsTuning.downedBleedoutSeconds;
            npc.combatTarget = ecs::Entity {};
            ctx.eventBus.dispatch({ gameplay::eventKind("OnDowned"),
                                    ecs::Entity {}, npc.entity,
                                    npc.factionTag });
            LOG_INFO("É3: {} is DOWN ({:.0f}s bleedout window)",
                     npc.editorId, ctx.statsTuning.downedBleedoutSeconds);
        } else if (!npc.downed && wasDowned) {
            npc.sitting = false; // back up: drop the kneel-proxy pose
        }
        if (npc.downed) {
            schedule_.releaseFurniture(ctx, npc);
            npc.attacking = false;
            npc.weaponDrawn = false;
            npc.blocking = false;
            npc.combatMove.reset();
            npc.sitting = true; // v1 kneel = the sit gate (see above)
            npc.path.clear();
            npc.speed = 0.0f;
            npc.anim->setParam("speed", 0.0f);
            npc.anim->update(dt, 0.0f);
            anim::bindPose(npc.rig->skeleton, npc.pose);
            npc.anim->evaluate(npc.pose);
            anim::skinMatrices(npc.rig->skeleton, npc.pose, npc.palette);
            npc.pendingAnimEvents.clear();
            continue;
        }

        // The NPC fights with the weapon it EQUIPPED (the loadout equips
        // the first weapon it rolled — the inventory link): stats, reach,
        // timings AND the drawn model all come from this one form.
        // banditWeapon stays the armed-and-equipmentless fallback.
        const data::WeaponForm* npcWeapon = ctx.banditWeapon;
        if (npc.entity.has<gameplay::Equipment>()) {
            const core::Guid equipped =
                npc.entity.get<gameplay::Equipment>().weapon;
            if (equipped.isValid()) {
                if (const auto* form =
                        ctx.forms.find<data::WeaponForm>(equipped)) {
                    npcWeapon = form;
                }
            }
        }
        npc.weaponModel = npcWeapon && npcWeapon->model.isValid()
                              ? npcWeapon->model
                              : core::Guid {};

        // Perception -> decision -> move lives in the combat
        // controller; true = combat overrode the schedule this frame.
        const bool inCombat = combat_.update(dt, ctx, npc, npcWeapon,
                                             playerSneaking, schedule_,
                                             npcByEntity_);

        if (!inCombat) {
            npc.blocking = false; // The fight is over, lower the guard
            npc.combatMove.reset(); // the intent trace re-logs next fight
        }
        // Drawn while fighting, back on the belt when it calms down —
        // extract reads this (the sim decides, the renderer shows).
        npc.weaponDrawn = inCombat;
        // An ACTIVE follower overrides his schedule with the
        // follow package (combat still wins the frame). Non-followers keep
        // the exact prior dispatch (iso-behavior). The « rester »
        // stance HOLDS him instead — active, standing at his spot; his
        // schedule takes over only on a DISMISS (v1 sandbox statement:
        // the home schedules are the town life of dismissed followers).
        bool following = false;
        bool staying = false;
        if (npc.entity.has<gameplay::FollowerState>()) {
            const auto& followerState =
                npc.entity.get<gameplay::FollowerState>();
            if (followerState.followerActive) {
                staying = gameplay::followerStance(followerState) ==
                          gameplay::FollowerStance::Stay;
                following = !staying;
            }
        }
        // Interruption/reprise: combat
        // and an OPEN dialogue override the schedule; the pure edge
        // detector stands the walker's state up on entry (drop the stale
        // path — combat already frees the seat where it must) and forces
        // an immediate re-evaluation on exit, so a fight spanning a
        // 10-min slot boundary resumes on the CURRENT entry.
        const bool inDialogue = ctx.dialoguePartner.is_alive() &&
                                ctx.dialoguePartner == npc.entity;
        switch (gameplay::updateInterruption(npc.scheduleInterrupted,
                                             inCombat || inDialogue)) {
        case gameplay::ScheduleSignal::Interrupted:
            npc.path.clear();
            npc.pathIndex = 0;
            break;
        case gameplay::ScheduleSignal::Resumed:
            schedule_.resume(npc);
            break;
        case gameplay::ScheduleSignal::None:
            break;
        }
        if (inCombat) {
            // combat overrode the schedule this frame
        } else if (inDialogue) {
            // held still while the player talks to him (seated stays
            // seated — only the walking stops)
        } else if (staying) {
            // stand where ordered (no follow, no schedule)
        } else if (following) {
            schedule_.followPlayer(dt, ctx, npc);
        } else if (npc.schedule.isValid()) {
            // --- Schedule-driven day ---
            schedule_.update(dt, ctx, npc, hourOfDay, idleDecay);
        } else if (patrolPoints.size() >= 2) {
            // --- Legacy patrol fallback ---
            schedule_.patrol(dt, ctx, npc, patrolPoints);
        }
        // Standing = no path AND no direct steering this frame (strafe
        // and flee move pathless — their run must reach the anim).
        npc.speed -= npc.speed * (1.0f - std::exp(-idleDecay * dt)) *
                     (npc.pathIndex >= npc.path.size() && !npc.steered
                          ? 1.0f
                          : 0.0f);
        npc.steered = false; // consumed — re-armed by the next steer

        // Anim: real speed feeds the param (transitions) AND the
        // referenceSpeed sync (anti-foot-sliding).
        npc.anim->setParam("speed", npc.speed);
        npc.anim->update(dt, npc.speed);
        anim::bindPose(npc.rig->skeleton, npc.pose);
        npc.anim->evaluate(npc.pose);
        anim::skinMatrices(npc.rig->skeleton, npc.pose, npc.palette);

        // The swing machine + the blade-touch hit + the guard
        // roll/mirror — after the pose evaluation (the hit segment
        // follows the hand joint through this frame's pose). : the
        // map resolves an NPC defender (combat target) back to its record.
        combat_.updateSwing(dt, ctx, npc, npcWeapon, playerSneaking,
                            npcByEntity_);

        // Drain the anim events the sink buffered onto
        // the bus — ONE kind ("AnimEvent"), the clip's name in `name`;
        // hit windows (routed above) and footsteps filter on it.
        for (str& name : npc.pendingAnimEvents) {
            gameplay::Event event;
            event.kind = gameplay::eventKind("AnimEvent");
            event.source = npc.entity;
            event.name = std::move(name);
            ctx.eventBus.dispatch(event);
        }
        npc.pendingAnimEvents.clear();
    }
}

void NpcDirector::extract(RenderSnapshot& out) const {
    for (const auto& npcPtr : npcs_) {
        const Npc& npc = *npcPtr;
        if (npc.vertices.id == 0 || !npc.entity.is_alive()) {
            continue;
        }
        const auto& transform = npc.entity.get<world::Transform>();
        const Mat4 world =
            glm::translate(Mat4 { 1.0f }, transform.position) *
            glm::mat4_cast(transform.rotation);
        out.skinned.push_back({ npc.entity.id(), world, npc.tint,
                                npc.vertices, npc.indices, npc.indexCount,
                                npc.palette });
        // A fighting NPC carries its EQUIPPED weapon in
        // hand_r — the very blade the hit test follows (blade-touch
        // combat). Drawn only while the sim says so (weaponDrawn);
        // kSwordGrip stands it up out of the fist; a raised guard turns
        // it oblique across the front.
        if (npc.weaponDrawn && !npc.dead && npc.handJoint >= 0) {
            anim::modelMatrices(npc.rig->skeleton, npc.pose, jointScratch);
            out.meshes.push_back(
                { npc.weaponModel.isValid() ? npc.weaponModel
                                            : swordMeshGuid(),
                  core::Guid {},
                  world *
                      jointScratch[static_cast<size_t>(npc.handJoint)] *
                      (npc.blocking ? kSwordGuardGrip : kSwordGrip) });
        }
    }
}

void NpcDirector::onNoise(const Vec3& position, f32 loudness) {
    // Hearing: every living perceiver within ITS hearing radius turns
    // toward the noise (Calm -> Suspicious; searches re-aim; Alert
    // ignores it). Dispatchers: player footsteps and combat cues (C4b).
    // Deliberately NOT a SpatialIndex query: the radius is per-
    // PERCEIVER (hearingRadius x loudness), so one shared-radius query
    // can't answer it — the plain sweep stays until NPC counts bite.
    for (auto& npcPtr : npcs_) {
        Npc& npc = *npcPtr;
        if (npc.dead || !npc.entity.is_alive() ||
            !npc.entity.has<world::Perception>()) {
            continue;
        }
        world::hearNoise(npc.entity.get_mut<world::Perception>(),
                         npc.entity.get<world::Transform>().position,
                         position, loudness);
    }
}

void NpcDirector::teardown(rhi::Device& device) {
    for (auto& npc : npcs_) {
        spawner_.destroyNpc(device, *npc);
    }
    npcs_.clear();
    npcByEntity_.clear();
    patrolPoints.clear();
    spawner_.clearRigs();
}

} // namespace game
