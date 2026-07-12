#include "game/scenes/NpcScheduleController.hpp"

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp> // angleAxis (the É1 face-the-player)

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"       // data::childrenOf
#include "engine/render/landscape/TerrainNoise.hpp" // terrain::height
#include "game/scenes/FollowerController.hpp" // teleportNear (É1)
#include "game/scenes/NpcDirector.hpp"    // Npc, NpcContext
#include "game/scenes/NpcMovement.hpp"    // moveNpcAlongPath/Direct, steerBlocked
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/actors/Followers.hpp"  // decideFollow (É1)
#include "gameplay/ability/GameplayEffects.hpp" // applyEffect/removeById (D1)
#include "gameplay/ai/AiForms.hpp"
#include "gameplay/ai/ScheduleSystem.hpp"
#include "gameplay/interaction/Furniture.hpp"
#include "gameplay/interaction/FurnitureForms.hpp" // FurnitureForm (D1)
#include "gameplay/stats/StatsTuning.hpp" // npcPatrolPauseSeconds
#include "world/ai/TerrainNavigator.hpp"
#include "world/scene/Components.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace game {

// Chantier 3 B3: re-evaluate the schedule every 10 game minutes; execute the
// active package (travel / wander / useFurniture / guard...).
void NpcScheduleController::updateNpcSchedule(const NpcContext& ctx, Npc& npc,
                                              f32 hourOfDay) {
    const i32 slot = static_cast<i32>(hourOfDay * 6.0f);
    if (slot == npc.lastEvaluatedSlot) {
        return;
    }
    npc.lastEvaluatedSlot = slot;
    const auto intent =
        gameplay::evaluateSchedule(ctx.forms, npc.schedule, hourOfDay);
    const gameplay::AiPackageForm* next = intent ? intent->package : nullptr;
    const core::Guid nextLocation =
        intent ? intent->location : core::Guid {};
    if (next != npc.activePackage || nextLocation != npc.activeLocation) {
        // Package change: stand up, drop the path, release furniture.
        npc.activePackage = next;
        npc.activeLocation = nextLocation;
        npc.intentReason = intent ? intent->reason : "(no schedule entry)";
        npc.path.clear();
        npc.pathIndex = 0;
        releaseFurniture(ctx, npc); // D1: occupancy + effect + gate
    }
}

void NpcScheduleController::releaseFurniture(const NpcContext& ctx,
                                             Npc& npc) {
    if (npc.furnitureClaimed) {
        ctx.furnitureOccupancy.release(npc.entity.id());
        npc.furnitureClaimed = false;
    }
    if (npc.furnitureEffectId != 0 &&
        npc.entity.has<gameplay::AbilitySystem>()) {
        // D1: standing up ends the furniture's effect (rest regen...).
        gameplay::removeEffectById(
            npc.entity.get_mut<gameplay::AbilitySystem>(),
            npc.furnitureEffectId, ctx.gameTags);
        npc.furnitureEffectId = 0;
    }
    npc.sitting = false;
}

void NpcScheduleController::update(f32 dt, const NpcContext& ctx, Npc& npc,
                                   f32 hourOfDay, f32& idleDecay) {
    auto& transform = npc.entity.get_mut<world::Transform>();
    updateNpcSchedule(ctx, npc, hourOfDay);
    npc.repathTimer -= dt;
    if (npc.wanderTimer > 0.0f) {
        npc.wanderTimer -= dt;
    }
    const gameplay::AiPackageForm* package = npc.activePackage;
    Vec3 anchor = transform.position;
    if (const auto* locationRef =
            npc.activeLocation.isValid()
                ? ctx.forms.find<world::ReferenceForm>(
                      npc.activeLocation)
                : nullptr) {
        anchor = locationRef->position;
        anchor.y = render::terrain::height(ctx.terrainParams, anchor.x,
                                           anchor.z);
    }
    const auto goTo = [&](const Vec3& target) {
        if (npc.pathIndex < npc.path.size() ||
            npc.repathTimer > 0.0f) {
            return;
        }
        const nav::PathResult found = ctx.navigator->findPath(
            { transform.position, target, 0.8f });
        npc.path = found.success ? found.waypoints : vector<Vec3> {};
        npc.pathIndex = 0;
        npc.repathTimer = 2.0f; // budget: no repath storm
    };
    const str kind = package ? package->kind : str { "guard" };
    if (kind == "wander") {
        const f32 radius = package ? package->radius : 4.0f;
        if (npc.pathIndex >= npc.path.size() &&
            npc.wanderTimer <= 0.0f) {
            // Cheap per-NPC stroll target around the anchor (cosmetic
            // randomness — not gameplay RNG, §8).
            const u32 hash =
                static_cast<u32>(npc.entity.id()) * 2654435761u +
                static_cast<u32>(ctx.timeSeconds * 0.37f);
            const f32 angle = static_cast<f32>(hash % 628) * 0.01f;
            const f32 reach =
                radius * (0.35f + static_cast<f32>(hash % 61) * 0.01f);
            goTo(anchor + Vec3 { std::cos(angle) * reach, 0.0f,
                                 std::sin(angle) * reach });
        }
        if (moveNpcAlongPath(ctx, npc, dt,
                             package ? package->speed : 1.0f)) {
            if (npc.pathIndex >= npc.path.size() &&
                npc.wanderTimer <= 0.0f && !npc.path.empty()) {
                npc.path.clear();
                npc.wanderTimer =
                    3.0f + static_cast<f32>(npc.entity.id() % 4);
            }
            idleDecay = 6.0f;
        }
    } else if (kind == "useFurniture" || kind == "sleep" ||
               kind == "eat" || kind == "work") {
        goTo(anchor);
        const bool pathDone = moveNpcAlongPath(
            ctx, npc, dt, package ? package->speed : 1.0f);
        Vec3 toAnchor = anchor - transform.position;
        toAnchor.y = 0.0f;
        const f32 anchorDist = glm::length(toAnchor);
        // The grid path often can't END on the furniture (the
        // prop is its own nav obstacle): close the last meters
        // DIRECTLY, collider-guarded — flush against the crate
        // counts as arrived (dev report 2026-07-12).
        bool arrived = pathDone && anchorDist <= 0.9f;
        if (pathDone && !arrived) {
            const Vec3 dir = toAnchor / glm::max(anchorDist, 1e-4f);
            if (steerBlocked(ctx, transform.position, dir)) {
                arrived = anchorDist <= 1.6f; // against the prop
            } else {
                moveNpcDirect(ctx, npc, dt, dir, 1.0f,
                              std::atan2(dir.x, dir.z));
            }
        }
        if (arrived) {
            // Claim a point and sit. D1: the claimed POINT's
            // animTag drives the anim gate ("State." + tag), and
            // the furniture's GAS effect (rest regen, warmth...)
            // applies for as long as the seat is held.
            if (!npc.furnitureClaimed) {
                npc.sitGate = "State.Sitting";
                const gameplay::FurnitureForm* furniture = nullptr;
                if (const auto* ref =
                        ctx.forms.find<world::ReferenceForm>(
                            npc.activeLocation)) {
                    furniture =
                        ctx.forms.find<gameplay::FurnitureForm>(
                            ref->baseForm);
                }
                u32 pointCount = 0;
                if (furniture) {
                    data::childrenOf<gameplay::FurniturePointForm>(
                        ctx.forms, furniture->id,
                        [&](const gameplay::FurniturePointForm&) {
                            ++pointCount;
                        });
                }
                const auto point = ctx.furnitureOccupancy.claim(
                    npc.activeLocation, glm::max(pointCount, 1u),
                    npc.entity.id());
                npc.furnitureClaimed = true;
                if (furniture) {
                    u32 index = 0;
                    Vec3 seatOffset { 0.0f };
                    data::childrenOf<gameplay::FurniturePointForm>(
                        ctx.forms, furniture->id,
                        [&](const gameplay::FurniturePointForm& p) {
                            if (point && index == *point) {
                                npc.sitGate = "State." + p.animTag;
                                seatOffset = p.offset;
                            }
                            ++index;
                        });
                    // Sit ON the point (the crate top), not
                    // beside it — release paths re-ground him.
                    transform.position = anchor + seatOffset;
                    if (furniture->effect.isValid()) {
                        if (const auto* effect =
                                ctx.forms
                                    .find<gameplay::EffectForm>(
                                        furniture->effect)) {
                            gameplay::applyEffect(
                                npc.entity.get_mut<
                                    gameplay::AttributeSet>(),
                                npc.entity.get_mut<
                                    gameplay::AbilitySystem>(),
                                *effect, ctx.gameTags, nullptr,
                                &npc.furnitureEffectId);
                        }
                    }
                }
            }
            npc.sitting = true;
        }
    } else { // travel / guard / unknown: reach the spot and stand
        goTo(anchor);
        moveNpcAlongPath(ctx, npc, dt, package ? package->speed : 1.0f);
    }
}

// FOLLOWERS É1: the follow package. Same building blocks as the schedule
// walker above — findPath + moveNpcAlongPath — pointed at the player by
// the pure decideFollow intent (gameplay/actors/Followers). Feel knobs =
// StatsTuningForm.follow* (§5).
void NpcScheduleController::followPlayer(f32 dt, const NpcContext& ctx,
                                         Npc& npc) {
    if (!ctx.playerEntity.is_alive() ||
        !ctx.playerEntity.has<world::Transform>()) {
        return;
    }
    auto& transform = npc.entity.get_mut<world::Transform>();
    if (npc.furnitureClaimed || npc.sitting) {
        releaseFurniture(ctx, npc); // recruited off a bench: stand up
    }
    const Vec3 playerPos =
        ctx.playerEntity.get<world::Transform>().position;
    const gameplay::FollowIntent intent = gameplay::decideFollow(
        transform.position, playerPos,
        gameplay::followTuning(ctx.statsTuning));
    npc.repathTimer -= dt;
    if (intent.teleport) {
        // Lost him (a door, a sprint across the ridge): pop in next to
        // the player — the same routine travel arrivals use.
        FollowerController::teleportNear(playerPos, ctx.terrainParams, npc);
        return;
    }
    if (intent.move) {
        if (npc.repathTimer <= 0.0f && ctx.navigator) {
            const nav::PathResult found = ctx.navigator->findPath(
                { transform.position, intent.target, 0.8f });
            npc.path = found.success ? found.waypoints : vector<Vec3> {};
            npc.pathIndex = 0;
            npc.repathTimer = ctx.statsTuning.followRepathSeconds;
        }
        moveNpcAlongPath(ctx, npc, dt, intent.speedScale);
        return;
    }
    // Near enough: stand and face the player (the moveNpcAlongPath yaw
    // smoothing, without the step).
    npc.path.clear();
    npc.pathIndex = 0;
    Vec3 to = playerPos - transform.position;
    to.y = 0.0f;
    if (glm::length(to) > 0.1f) {
        const f32 goalYaw = std::atan2(to.x, to.z);
        f32 delta = goalYaw - npc.yaw;
        while (delta > glm::pi<f32>()) {
            delta -= glm::two_pi<f32>();
        }
        while (delta < -glm::pi<f32>()) {
            delta += glm::two_pi<f32>();
        }
        npc.yaw += delta * (1.0f - std::exp(-8.0f * dt));
        transform.rotation =
            glm::angleAxis(npc.yaw, Vec3 { 0.0f, 1.0f, 0.0f });
    }
}

void NpcScheduleController::patrol(f32 dt, const NpcContext& ctx, Npc& npc,
                                   const vector<Vec3>& patrolPoints) {
    const auto& transform = npc.entity.get<world::Transform>();
    const Vec3 goal = patrolPoints[npc.target % patrolPoints.size()];
    Vec3 to = goal - transform.position;
    to.y = 0.0f;
    const f32 distance = glm::length(to);
    if (npc.pauseTimer > 0.0f) {
        npc.pauseTimer -= dt;
    } else if (distance < 0.4f) {
        npc.pauseTimer = ctx.statsTuning.npcPatrolPauseSeconds;
        npc.target =
            (npc.target + 1) % static_cast<u32>(patrolPoints.size());
    } else {
        npc.path = { goal };
        npc.pathIndex = 0;
        moveNpcAlongPath(ctx, npc, dt, 1.0f);
    }
}

} // namespace game
