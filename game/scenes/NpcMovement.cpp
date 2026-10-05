#include "game/scenes/NpcMovement.hpp"

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/physics/Physics.hpp"     // phys::PhysicsWorld/RayHit
#include "engine/render/landscape/TerrainNoise.hpp" // terrain::height
#include "engine/terrain/WindField.hpp"   // the gusts
#include "game/scenes/NpcDirector.hpp"    // Npc, NpcContext
#include "game/scenes/SpiritDirector.hpp" // fireBurningAt
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp" // attr, currentValueOf
#include "gameplay/stats/StatsTuning.hpp"  // movementSpeedScale3D
#include "world/scene/Components.hpp"

namespace game {

// Contract in the header.
bool groundAt(const render::TerrainParams& terrain, bool interiorMode,
              phys::PhysicsWorld* physics, Vec3& position) {
    if (!interiorMode) {
        position.y =
            render::terrain::height(terrain, position.x, position.z);
        return true;
    }
    if (!physics) {
        return true; // headless: authored y is trusted
    }
    // 1.8 = 1.2 above + 0.6 below the feet: one step's worth. A longer
    // probe let an NPC ground onto surfaces meters below through a gap
    // (a shelf in a lower room's ceiling) instead of refusing the ledge.
    const phys::RayHit hit =
        physics->rayCast(position + Vec3 { 0.0f, 1.2f, 0.0f },
                         { 0.0f, -1.0f, 0.0f }, 1.8f);
    // An UPWARD snap past the step budget is a wall, not a step: the
    // cavern walls flare at the base, and accepting those hits walked
    // NPCs up the flare and into the rock.
    if (!hit.hit || hit.position.y > position.y + 0.5f) {
        return false;
    }
    position.y = hit.position.y;
    return true;
}

f32 wrapAngle(f32 angle) {
    while (angle > glm::pi<f32>()) {
        angle -= glm::two_pi<f32>();
    }
    while (angle < -glm::pi<f32>()) {
        angle += glm::two_pi<f32>();
    }
    return angle;
}

void smoothYawToward(f32& yaw, f32 goalYaw, f32 rate, f32 dt) {
    yaw += wrapAngle(goalYaw - yaw) * (1.0f - std::exp(-rate * dt));
}

bool groundNpc(const NpcContext& ctx, Vec3& position) {
    return groundAt(ctx.terrainParams, ctx.interiorMode, ctx.physics,
                    position);
}

namespace {

// Kinematic depenetration for cavern walls: eight chest-height probes; a
// hit inside the capsule's radius shoves the NPC back out along the probe.
// The step-refusal gates stop them from ENTERING head-on, this backs them
// off the grazing overlaps those rays can't see (curved walls, corner-
// cutting between waypoints). Interior-only: the noisy tube walls are the
// case; outdoors the blockers are authored boxes steerBlocked handles.
void resolveWallOverlap(const NpcContext& ctx, Vec3& position) {
    if (!ctx.interiorMode || !ctx.physics) {
        return;
    }
    constexpr f32 kRadius = 0.5f; // capsule + skin margin
    for (u32 i = 0; i < 8; ++i) {
        const f32 a = static_cast<f32>(i) * (glm::two_pi<f32>() / 8.0f);
        const Vec3 dir { std::cos(a), 0.0f, std::sin(a) };
        const Vec3 chest = position + Vec3 { 0.0f, 0.9f, 0.0f };
        const phys::RayHit hit = ctx.physics->rayCast(chest, dir, kRadius);
        if (hit.hit) {
            position -= dir * (kRadius - hit.distance);
        }
    }
}

} // namespace

bool moveNpcAlongPath(const NpcContext& ctx, Npc& npc, f32 dt,
                      f32 speedScale) {
    if (npc.pathIndex >= npc.path.size()) {
        return true;
    }
    auto& transform = npc.entity.get_mut<world::Transform>();
    const auto& sys = npc.entity.get<gameplay::AbilitySystem>();
    const f32 walkSpeed =
        gameplay::currentValueOf(sys, gameplay::attr("movementSpeed")) *
        ctx.statsTuning.movementSpeedScale3D * ctx.statsTuning.npcWalkFactor *
        speedScale; // §5-tunable

    const Vec3 goal = npc.path[npc.pathIndex];
    Vec3 to = goal - transform.position;
    to.y = 0.0f;
    const f32 distance = glm::length(to);
    if (distance < 0.35f) {
        ++npc.pathIndex;
        return npc.pathIndex >= npc.path.size();
    }
    const Vec3 dir = to / distance;
    const Vec3 before = transform.position;
    transform.position += dir * glm::min(walkSpeed * dt, distance);
    resolveWallOverlap(ctx, transform.position);
    if (!groundNpc(ctx, transform.position)) {
        transform.position = before; // ledge: hold the edge, keep facing
    }
        transform.position.y += npc.airHeight; // thrown: over the ground
    smoothYawToward(npc.yaw, std::atan2(dir.x, dir.z), 8.0f, dt);
    transform.rotation = glm::angleAxis(npc.yaw, Vec3 { 0.0f, 1.0f, 0.0f });
    npc.speed += (walkSpeed - npc.speed) * (1.0f - std::exp(-10.0f * dt));
    return false;
}

void moveNpcDirect(const NpcContext& ctx, Npc& npc, f32 dt,
                   const Vec3& direction, f32 speedScale, f32 faceYaw) {
    auto& transform = npc.entity.get_mut<world::Transform>();
    const auto& sys = npc.entity.get<gameplay::AbilitySystem>();
    const f32 walkSpeed =
        gameplay::currentValueOf(sys, gameplay::attr("movementSpeed")) *
        ctx.statsTuning.movementSpeedScale3D * ctx.statsTuning.npcWalkFactor *
        speedScale;
    const Vec3 before = transform.position;
    transform.position += direction * walkSpeed * dt;
    resolveWallOverlap(ctx, transform.position);
    if (!groundNpc(ctx, transform.position)) {
        transform.position = before; // ledge: hold the edge, keep facing
    }
        transform.position.y += npc.airHeight; // thrown: over the ground
    npc.yaw = faceYaw;
    transform.rotation = glm::angleAxis(npc.yaw, Vec3 { 0.0f, 1.0f, 0.0f });
    npc.speed += (walkSpeed - npc.speed) * (1.0f - std::exp(-10.0f * dt));
    npc.steered = true; // pathless but MOVING: skip the idle speed decay
}

bool steerBlocked(const NpcContext& ctx, const Vec3& from,
                  const Vec3& direction) {
    if (!ctx.physics) {
        return false;
    }
    // Three rays across the capsule's width: the single center ray ran
    // parallel to a curving wall while the shoulder sank into it.
    const Vec3 chest = from + Vec3 { 0.0f, 0.9f, 0.0f };
    const Vec3 flat { direction.z, 0.0f, -direction.x };
    const f32 flatLen = glm::length(flat);
    const Vec3 side =
        flatLen > 0.001f ? flat * (0.35f / flatLen) : Vec3 { 0.0f };
    for (const Vec3& origin : { chest - side, chest, chest + side }) {
        if (ctx.physics->rayCast(origin, direction, 0.9f).hit) {
            return true;
        }
    }
    return false;
}

// Contract in the header.
bool steerFromFire(const NpcContext& ctx, Npc& npc, f32 dt) {
    if (!ctx.spirits || ctx.interiorMode || ctx.spirits->fireIdle()) {
        return false;
    }
    const Vec3 pos = npc.entity.get<world::Transform>().position;
    // Three rings of samples: every burning one pushes away, the near
    // ones harder.
    Vec2 push { 0.0f };
    u32 hits = 0;
    for (i32 ring = 1; ring <= 3; ++ring) {
        const f32 r = kFireFleeRadius * static_cast<f32>(ring) / 3.0f;
        for (i32 k = 0; k < 12; ++k) {
            const f32 a = static_cast<f32>(k) * (glm::two_pi<f32>() / 12.0f);
            const Vec2 d { std::cos(a), std::sin(a) };
            if (ctx.spirits->fireBurningAt(pos.x + d.x * r, pos.z + d.y * r)) {
                push -= d / r;
                ++hits;
            }
        }
    }
    const bool underfoot = ctx.spirits->fireBurningAt(pos.x, pos.z);
    if (hits == 0 && !underfoot) {
        return false;
    }
    // Surrounded evenly (or only the ground underfoot burns): straight
    // ahead is as good a way out as any.
    Vec2 dir = glm::length(push) > 1e-3f
                   ? glm::normalize(push)
                   : Vec2 { std::sin(npc.yaw), std::cos(npc.yaw) };
    Vec3 away { dir.x, 0.0f, dir.y };
    if (steerBlocked(ctx, pos, away)) {
        for (const f32 turn : { 1.2f, -1.2f }) {
            const Vec3 side = glm::angleAxis(turn, Vec3 { 0.0f, 1.0f, 0.0f }) * away;
            if (!steerBlocked(ctx, pos, side)) {
                away = side;
                break;
            }
        }
    }
    npc.path.clear();
    npc.pathIndex = 0;
    moveNpcDirect(ctx, npc, dt, away, 1.35f, std::atan2(away.x, away.z));
    return true;
}

void pushNpcByWind(const NpcContext& ctx, Npc& npc, f32 dt) {
    if (!ctx.wind || ctx.interiorMode) {
        return;
    }
    auto& transform = npc.entity.get_mut<world::Transform>();
    const Vec2 gust =
        ctx.wind->windAt(transform.position.x, transform.position.z) -
        ctx.wind->globalDir * ctx.wind->globalSpeed;
    if (glm::dot(gust, gust) < 0.25f) {
        return; // under 0.5 m/s: a breath, not a shove
    }
    Vec3 next = transform.position +
                Vec3 { gust.x, 0.0f, gust.y } * (kNpcWindPush * dt);
    if (groundNpc(ctx, next)) {
        transform.position = next;
    }
}

void applyNpcShove(const NpcContext& ctx, Npc& npc, f32 dt) {
    if (glm::dot(npc.shove, npc.shove) < 0.01f) {
        npc.shove = Vec2 { 0.0f };
        return;
    }
    auto& transform = npc.entity.get_mut<world::Transform>();
    Vec3 next = transform.position + Vec3 { npc.shove.x, 0.0f, npc.shove.y } * dt;
    if (groundNpc(ctx, next)) {
        transform.position = next;
    }
    npc.shove *= std::exp(-dt / kNpcShoveDecay);
}

} // namespace game
