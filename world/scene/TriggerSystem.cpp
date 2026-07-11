#include "world/scene/TriggerSystem.hpp"

#include <algorithm>
#include <cstddef>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "gameplay/event/EventBus.hpp"
#include "world/scene/Components.hpp"
#include "world/scene/SpatialIndex.hpp" // B1: the near-actors fast path

namespace world {

namespace {

bool insideOrientedBox(const Vec3& point, const Transform& box,
                       const Vec3& halfExtents) {
    // Into box-local space: un-rotate the offset, compare to scaled half
    // extents (component-wise — the box scales with its reference).
    const Vec3 local = glm::inverse(box.rotation) * (point - box.position);
    const Vec3 half = halfExtents * box.scale;
    return std::abs(local.x) <= half.x && std::abs(local.y) <= half.y &&
           std::abs(local.z) <= half.z;
}

} // namespace

void updateTriggerVolumes(ecs::World& world, const TriggerCallbacks& cb,
                          const SpatialIndex* index) {
    // Snapshot both sides before dispatching: handlers/scripts may add or
    // remove entities, which must not perturb THIS tick's iteration.
    // (With an index the snapshot was taken at its rebuild.)
    struct Actor {
        ecs::Entity entity;
        Vec3 position;
    };
    vector<Actor> actors;
    if (!index) {
        world.handle()
            .query_builder<const Transform>()
            .with<ActorMarker>()
            .build()
            .each([&](flecs::entity entity, const Transform& transform) {
                actors.push_back({ entity, transform.position });
            });
    }
    vector<SpatialIndex::Entry> nearby; // per-volume scratch (B1 path)

    vector<ecs::Entity> triggers;
    world.handle()
        .query_builder<const Transform, const TriggerVolume>()
        .build()
        .each([&](flecs::entity entity, const Transform&,
                  const TriggerVolume&) { triggers.push_back(entity); });

    for (ecs::Entity trigger : triggers) {
        if (!trigger.is_alive()) {
            continue; // a handler despawned it earlier this tick
        }
        // Work on COPIES: dispatch below may add/remove components (an
        // archetype change moves storage — the U8-4 lesson), so no
        // component reference is ever held across a callback.
        const Transform transform = trigger.get<Transform>();
        const TriggerVolume volume = trigger.get<TriggerVolume>();
        TriggerOccupancy occupancy = trigger.ensure<TriggerOccupancy>();
        bool fired = volume.fired;

        // B1: with an index, only the actors within the box's bounding
        // sphere are candidates — anyone further out is outside for sure.
        vector<Actor> candidates;
        if (index) {
            nearby.clear();
            index->queryRadius(transform.position,
                               glm::length(volume.halfExtents *
                                           transform.scale),
                               nearby);
            candidates.reserve(nearby.size());
            for (const SpatialIndex::Entry& entry : nearby) {
                candidates.push_back({ entry.entity, entry.position });
            }
        }
        const vector<Actor>& tested = index ? candidates : actors;

        for (const Actor& actor : tested) {
            if (!actor.entity.is_alive()) {
                continue;
            }
            const bool now = insideOrientedBox(actor.position, transform,
                                               volume.halfExtents);
            const u64 id = actor.entity.id();
            const auto it = std::find(occupancy.inside.begin(),
                                      occupancy.inside.end(), id);
            const bool was = it != occupancy.inside.end();
            if (now == was) {
                continue;
            }
            if (now) {
                occupancy.inside.push_back(id);
            } else {
                occupancy.inside.erase(it);
            }
            // A fired once-trigger stays silent forever (the latch is
            // reflected — a save keeps it fired, §5).
            if (volume.once && fired) {
                continue;
            }
            if (cb.events && !volume.event.empty()) {
                gameplay::Event event;
                event.kind = gameplay::eventKind(volume.event);
                event.name = volume.event;
                event.source = actor.entity;
                event.target = trigger;
                event.value = now ? 1.0f : 0.0f;
                cb.events->dispatch(event);
            }
            if (now && cb.runScript && !volume.script.empty()) {
                cb.runScript(volume.script, actor.entity, trigger);
            }
            if (now && volume.once) {
                fired = true;
            }
        }

        // B1 leave-sweep: an occupant no longer among the candidates left
        // the neighborhood entirely — that IS a leave (the full scan got
        // this for free by testing every actor).
        if (index) {
            for (size_t i = occupancy.inside.size(); i-- > 0;) {
                const u64 id = occupancy.inside[i];
                const bool seen =
                    std::any_of(tested.begin(), tested.end(),
                                [&](const Actor& actor) {
                                    return actor.entity.id() == id;
                                });
                if (seen) {
                    continue;
                }
                const ecs::Entity actor { world.handle().get_alive(id) };
                if (!actor.is_alive()) {
                    continue; // despawned occupants linger (full-scan parity)
                }
                occupancy.inside.erase(occupancy.inside.begin() +
                                       static_cast<std::ptrdiff_t>(i));
                if (volume.once && fired) {
                    continue;
                }
                if (cb.events && !volume.event.empty()) {
                    gameplay::Event event;
                    event.kind = gameplay::eventKind(volume.event);
                    event.name = volume.event;
                    event.source = actor;
                    event.target = trigger;
                    event.value = 0.0f;
                    cb.events->dispatch(event);
                }
            }
        }

        if (trigger.is_alive()) { // a handler may have despawned it
            trigger.ensure<TriggerOccupancy>() = std::move(occupancy);
            if (fired != volume.fired) {
                trigger.get_mut<TriggerVolume>().fired = fired;
            }
        }
    }
}

} // namespace world
