#include "world/scene/Floaters.hpp"

#include <cmath>

#include "world/scene/Components.hpp"

namespace world {

void updateFloaters(ecs::World& world, f32 dt, f32 timeSeconds,
                    const WaterSurfaceFn& surface,
                    const WaterFlowFn& flow) {
    if (!surface) {
        return;
    }
    world.handle()
        .query_builder<Transform, const Floater>()
        .build()
        .each([&](flecs::entity e, Transform& transform,
                  const Floater& floater) {
            const auto level = surface(transform.position);
            if (!level) {
                return; // dry: rest where it is
            }
            if (flow && floater.driftFactor != 0.0f) {
                const Vec2 current = flow(transform.position);
                transform.position.x +=
                    current.x * floater.driftFactor * dt;
                transform.position.z +=
                    current.y * floater.driftFactor * dt;
            }
            // Ride the surface with a small deterministic bob (phase
            // from the entity id — no RNG, replays stay exact).
            const f32 phase =
                static_cast<f32>(e.id() % 628u) * 0.01f;
            transform.position.y =
                *level - floater.draft +
                floater.bobAmplitude *
                    std::sin(timeSeconds * 1.7f + phase);
        });
}

} // namespace world
