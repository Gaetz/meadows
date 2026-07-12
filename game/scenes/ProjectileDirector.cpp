#include "game/scenes/ProjectileDirector.hpp"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/core/Log.hpp"
#include "engine/physics/Physics.hpp"
#include "game/SceneSubmit.hpp"
#include "game/WeaponMeshes.hpp" // arrowMeshGuid
#include "game/scenes/NpcDirector.hpp" // Npc
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/combat/MeleeSwing.hpp" // segmentHitsCapsule
#include "gameplay/cue/GameplayCues.hpp"
#include "gameplay/event/EventBus.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "world/scene/Components.hpp"

namespace game {

namespace {

// The one damage application for whatever the arrow found.
void strike(const ProjectileContext& ctx, ecs::Entity target,
            const gameplay::Projectile& arrow, const Vec3& at) {
    gameplay::StatBlock block {
        target.get_mut<gameplay::CoreAttributes>(),
        target.get_mut<gameplay::AttributeSet>(),
        target.get_mut<gameplay::AbilitySystem>(),
        target.get_mut<gameplay::CombatState>()
    };
    gameplay::DamageEvent event = arrow.payload; // captured at fire time
    const gameplay::DamageResult result =
        gameplay::applyDamage(block, event, ctx.gameTags,
                              ctx.derivedStats, nullptr, ctx.statsTuning);
    LOG_INFO("A7: arrow hits for {:.0f}", result.healthDamage);
    gameplay::Event hit;
    hit.kind = gameplay::eventKind("OnHitTaken");
    hit.source = ecs::Entity {}; // the shooter may be long gone
    hit.target = target;
    hit.value = result.healthDamage;
    ctx.eventBus.dispatch(hit);
    if (ctx.cues) {
        const gameplay::DamageType type =
            event.channels.empty() ? gameplay::DamageType::Pierce
                                   : event.channels[0].type;
        ctx.cues->emit({ str { "Cue.Hit." } +
                             gameplay::damageTypeName(type),
                         at, result.healthDamage });
    }
}

} // namespace

void ProjectileDirector::update(f32 dt, const ProjectileContext& ctx) {
    for (gameplay::Projectile& arrow : projectiles) {
        const Vec3 from = gameplay::stepProjectile(arrow, dt);
        if (arrow.planted) {
            continue;
        }
        const Vec3 sweep = arrow.position - from;
        const f32 sweepLen = glm::length(sweep);
        if (sweepLen < 1e-5f) {
            continue;
        }
        const Vec3 dir = sweep / sweepLen;

        // Static world first: the closest of both hit kinds should win,
        // but a world hit inside the same step as an actor hit is rare
        // enough that actor-first keeps the code flat. [cpp-tuning]
        // Actors: analytic capsules (outside the broadphase).
        bool consumed = false;
        constexpr f32 kRadius = 0.4f;
        for (const auto& npcPtr : ctx.npcs) {
            const Npc& npc = *npcPtr;
            if (npc.dead || !npc.entity.is_alive() ||
                npc.entity.id() == arrow.shooter) {
                continue;
            }
            const Vec3 feet = npc.entity.get<world::Transform>().position;
            if (gameplay::segmentHitsCapsule(
                    from, arrow.position,
                    feet + Vec3 { 0.0f, kRadius, 0.0f },
                    feet + Vec3 { 0.0f, 1.8f - kRadius, 0.0f }, kRadius)) {
                strike(ctx, npc.entity, arrow, arrow.position);
                consumed = true;
                break;
            }
        }
        // The player is a target too (NPC archers) — god mode excluded.
        if (!consumed && ctx.player && ctx.playerEntity.is_alive() &&
            ctx.playerEntity.id() != arrow.shooter && !ctx.godMode) {
            const Vec3 feet = ctx.player->position();
            const f32 height =
                ctx.player->isCrouched() ? 0.9f : 1.8f; // half the target
            if (gameplay::segmentHitsCapsule(
                    from, arrow.position,
                    feet + Vec3 { 0.0f, kRadius, 0.0f },
                    feet + Vec3 { 0.0f, height - kRadius, 0.0f },
                    kRadius)) {
                strike(ctx, ctx.playerEntity, arrow, arrow.position);
                consumed = true;
            }
        }
        if (consumed) {
            arrow.ttl = 0.0f; // the sweep below removes it
            continue;
        }
        if (ctx.physics) {
            const phys::RayHit hit =
                ctx.physics->rayCast(from, dir, sweepLen);
            if (hit.hit) {
                // Planted: the mesh stays where it struck, nose in.
                arrow.position = hit.position;
                arrow.planted = true;
            }
        }
    }
    projectiles.erase(
        std::remove_if(projectiles.begin(), projectiles.end(),
                       [](const gameplay::Projectile& arrow) {
                           return gameplay::projectileExpired(arrow);
                       }),
        projectiles.end());
}

void ProjectileDirector::extract(RenderSnapshot& out) const {
    for (const gameplay::Projectile& arrow : projectiles) {
        // Orient the +Y mesh along the flight (planted: the last one).
        const Vec3 dir = glm::dot(arrow.velocity, arrow.velocity) > 1e-6f
                             ? glm::normalize(arrow.velocity)
                             : Vec3 { 0.0f, 1.0f, 0.0f };
        const Vec3 up { 0.0f, 1.0f, 0.0f };
        Quat rotation { 1.0f, 0.0f, 0.0f, 0.0f };
        const f32 cosA = glm::clamp(glm::dot(up, dir), -1.0f, 1.0f);
        if (cosA < 0.9999f) {
            const Vec3 axis =
                cosA > -0.9999f
                    ? glm::normalize(glm::cross(up, dir))
                    : Vec3 { 1.0f, 0.0f, 0.0f }; // straight down
            rotation = glm::angleAxis(std::acos(cosA), axis);
        }
        // The nock trails: shift back so the TIP sits at `position`.
        const Vec3 base = arrow.position - dir * 0.55f;
        out.meshes.push_back({ arrowMeshGuid(), core::Guid {},
                               glm::translate(Mat4 { 1.0f }, base) *
                                   glm::mat4_cast(rotation) });
    }
}

} // namespace game
