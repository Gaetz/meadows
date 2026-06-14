#include "world/scene/Collision.hpp"

#include <cmath>

#include "world/scene/Components.hpp"

namespace world {

bool aabbOverlap(Vec2 posA, Vec2 halfA, Vec2 posB, Vec2 halfB) {
    return std::abs(posA.x - posB.x) < (halfA.x + halfB.x) &&
           std::abs(posA.y - posB.y) < (halfA.y + halfB.y);
}

Vec2 minimumTranslation(Vec2 posA, Vec2 halfA, Vec2 posB, Vec2 halfB) {
    const Vec2 delta = posA - posB;
    const f32 overlapX = (halfA.x + halfB.x) - std::abs(delta.x);
    const f32 overlapY = (halfA.y + halfB.y) - std::abs(delta.y);
    if (overlapX <= 0.0f || overlapY <= 0.0f) {
        return { 0.0f, 0.0f };
    }
    if (overlapX < overlapY) {
        return { delta.x < 0.0f ? -overlapX : overlapX, 0.0f };
    }
    return { 0.0f, delta.y < 0.0f ? -overlapY : overlapY };
}

namespace {

struct Box {
    ecs::Entity entity;
    Vec2 position;
    Vec2 half;
};

} // namespace

void resolveCollisions(ecs::World& world) {
    vector<Box> solids;
    world.handle().query<const Transform, const Collider>().each(
        [&](flecs::entity entity, const Transform& transform,
            const Collider& collider) {
            if (!collider.trigger) {
                solids.push_back({ entity,
                                   { transform.position.x, transform.position.y },
                                   collider.halfExtents });
            }
        });

    world.handle()
        .query<Transform, const Collider, const Velocity>()
        .each([&](flecs::entity entity, Transform& transform,
                  const Collider& collider, const Velocity&) {
            if (collider.trigger) {
                return;
            }
            Vec2 position { transform.position.x, transform.position.y };
            for (const Box& solid : solids) {
                if (solid.entity == entity) {
                    continue;
                }
                if (aabbOverlap(position, collider.halfExtents, solid.position,
                                solid.half)) {
                    position += minimumTranslation(position, collider.halfExtents,
                                                   solid.position, solid.half);
                }
            }
            transform.position.x = position.x;
            transform.position.y = position.y;
        });
}

void forEachTriggerOverlap(
    ecs::World& world,
    const std::function<void(ecs::Entity, ecs::Entity)>& onOverlap) {
    vector<Box> triggers;
    world.handle().query<const Transform, const Collider>().each(
        [&](flecs::entity entity, const Transform& transform,
            const Collider& collider) {
            if (collider.trigger) {
                triggers.push_back({ entity,
                                     { transform.position.x, transform.position.y },
                                     collider.halfExtents });
            }
        });

    world.handle()
        .query<const Transform, const Collider, const Velocity>()
        .each([&](flecs::entity entity, const Transform& transform,
                  const Collider& collider, const Velocity&) {
            const Vec2 position { transform.position.x, transform.position.y };
            for (const Box& trigger : triggers) {
                if (trigger.entity == entity) {
                    continue;
                }
                if (aabbOverlap(position, collider.halfExtents, trigger.position,
                                trigger.half)) {
                    onOverlap(entity, trigger.entity);
                }
            }
        });
}

} // namespace world
