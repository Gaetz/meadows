#pragma once

#include <functional>

#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp"  // ecs::Entity
#include "game/InputActions.hpp" // game::ActionMap
#include "game/Settings.hpp"     // game::Settings (look feel)

namespace platform {
class Input;
}
namespace render {
class FlyCamera;
struct TerrainParams;
}
namespace gameplay {
struct StatsTuningForm;
}

namespace game {

// The mount tech proof (docs/FOLLOWERS.md §8): mount /
// dismount + ridden movement + the mount speed stat, NOTHING else (no
// whistle, stable, combat behaviors or follower mounts — all later).
// The containment contract: while mounted the player's
// CharacterBody is DESTROYED (the travel precedent) and the scene calls
// THIS controller INSTEAD of PlayerController — one if/else at the call
// site, zero changes inside PlayerController. The ride owns the camera,
// the mount's Transform and the player entity's Transform (so followers,
// nameplate consumers and the extract keep working) until dismount
// respawns the capsule beside the mount.
struct RideContext {
    render::FlyCamera& flyCamera;
    platform::Input& input;
    const ActionMap* actions;   // Interact = dismount
    const Settings* settings;   // look sensitivity / invert / stick
    const gameplay::StatsTuningForm& statsTuning; // eye/accel tuning
    const render::TerrainParams& terrainParams;   // the ground truth
    ecs::Entity playerEntity;
    // Scene action: respawn the player capsule with its feet HERE (the
    // spawnBody idiom — dismount, or the auto-bailout when the mount
    // entity dies under us, e.g. its cell streamed out).
    std::function<void(const Vec3& feet)> spawnPlayerBody;
};

class RideController {
public:
    bool mounted() const { return mount_.is_alive(); }

    // [E] on the pony (PromptKind::Mount): the scene destroyed the
    // capsule and hands over. `mountSpeed` comes from the FurnitureForm.
    void mount(ecs::Entity mount, f32 mountSpeed);

    // Respawn the capsule beside the mount and release it. Also the
    // guard travel / mode exits call before doing their own thing.
    void dismount(const RideContext& ctx);

    // onEnter/onExit re-init: drop the mount without any respawn (the
    // scene rebuilds the world around us).
    void reset();

    // Per frame in Play INSTEAD of PlayerController.update while
    // mounted: own mouselook, wish -> stepRide, transforms + camera,
    // [E] polls the dismount.
    void update(f32 dt, const RideContext& ctx);

private:
    ecs::Entity mount_ {};
    f32 mountSpeed_ { 9.0f };
    Vec3 velocity { 0.0f };  // smoothed horizontal velocity (m/s)
    f32 mountYaw_ { 0.0f };  // the mount's facing (smoothed toward travel)
    // The [E] that mounts and the dismount poll read the same pressed
    // edge in the same frame — swallow it once.
    bool justMounted_ { false };
};

} // namespace game
