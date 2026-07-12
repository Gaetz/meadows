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
#include "gameplay/combat/MeleeStrike.hpp" // resolveStrikeDamage
#include "gameplay/combat/MeleeSwing.hpp"  // segmentHitsActor
#include "gameplay/cue/GameplayCues.hpp"
#include "gameplay/event/EventBus.hpp"
#include "gameplay/inventory/Inventory.hpp" // arrow pickup (A7+)
#include "gameplay/stats/CoreAttributes.hpp"
#include "world/scene/Components.hpp"

namespace game {

namespace {

// The one damage application for whatever the arrow found — the shared
// strike tail (damage -> events -> cue); arrows skip the guard stage.
void strike(const ProjectileContext& ctx, ecs::Entity target,
            const gameplay::Projectile& arrow, const Vec3& at) {
    gameplay::StatBlock defender {
        target.get_mut<gameplay::CoreAttributes>(),
        target.get_mut<gameplay::AttributeSet>(),
        target.get_mut<gameplay::AbilitySystem>(),
        target.get_mut<gameplay::CombatState>()
    };
    const gameplay::StrikeContext strikeCtx {
        ctx.gameTags, ctx.derivedStats, ctx.statsTuning, &ctx.eventBus,
        ctx.cues
    };
    // Source = empty: the shooter may be long gone (payload captured at
    // fire time).
    const gameplay::DamageResult result = gameplay::resolveStrikeDamage(
        defender, ecs::Entity {}, target, arrow.payload, at, strikeCtx);
    LOG_INFO("A7: arrow hits for {:.0f}", result.healthDamage);
}

} // namespace

void ProjectileDirector::update(f32 dt, const ProjectileContext& ctx) {
    for (gameplay::Projectile& arrow : projectiles) {
        const Vec3 from = gameplay::stepProjectile(arrow, dt);
        if (arrow.planted) {
            // A7+: walking over a planted arrow returns its ammo item to
            // the bag — anyone's arrow (an archer's misses are loot).
            if (arrow.ammoItem.isValid() && ctx.player &&
                ctx.playerEntity.is_alive() &&
                ctx.playerEntity.has<gameplay::Inventory>()) {
                const Vec3 gap = arrow.position - ctx.player->position();
                const f32 reach = ctx.statsTuning.arrowPickupRadius;
                if (glm::dot(gap, gap) < reach * reach) {
                    gameplay::addItem(
                        ctx.playerEntity.get_mut<gameplay::Inventory>(),
                        arrow.ammoItem, 1);
                    arrow.plantedTtl = 0.0f; // picked: gone this frame
                    LOG_INFO("A7: arrow recovered");
                }
            }
            continue;
        }
        const Vec3 sweep = arrow.position - from;
        const f32 sweepLen = glm::length(sweep);
        if (sweepLen < 1e-5f) {
            continue;
        }
        const Vec3 dir = sweep / sweepLen;

        // TODO(P1): actor-first + first-in-iteration is a known
        // correctness compromise — an arrow should bury in a wall
        // between shooter and NPC (world hit in the same step loses to
        // the actor hit), and among several actors on the sweep the
        // first in list order wins over the closest.
        // Actors: analytic capsules (outside the broadphase).
        bool consumed = false;
        for (const auto& npcPtr : ctx.npcs) {
            const Npc& npc = *npcPtr;
            if (npc.dead || !npc.entity.is_alive() ||
                npc.entity.id() == arrow.shooter) {
                continue;
            }
            const Vec3 feet = npc.entity.get<world::Transform>().position;
            if (gameplay::segmentHitsActor(from, arrow.position, feet)) {
                strike(ctx, npc.entity, arrow, arrow.position);
                consumed = true;
                break;
            }
        }
        // The player is a target too (NPC archers) — god mode excluded;
        // a crouched player is half the target.
        if (!consumed && ctx.player && ctx.playerEntity.is_alive() &&
            ctx.playerEntity.id() != arrow.shooter && !ctx.godMode) {
            if (gameplay::segmentHitsActor(from, arrow.position,
                                           ctx.player->position(),
                                           ctx.player->isCrouched())) {
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
