#include "game/scenes/RideController.hpp"
#include "game/scenes/NpcMovement.hpp"

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/platform/Input.hpp"
#include "engine/render/FlyCamera.hpp"
#include "engine/render/landscape/TerrainNoise.hpp" // terrain::height
#include "gameplay/actors/Riding.hpp"               // stepRide (headless)
#include "gameplay/stats/StatsTuning.hpp"
#include "world/scene/Components.hpp" // world::Transform

namespace game {

namespace {

// Saddle geometry over the mount's ground position — matches the
// makeHorseMesh proportions. [cpp-tuning]
constexpr f32 kSaddleHeight = 1.3f;   // where the rider sits
constexpr f32 kRideEyeHeight = 2.2f;  // camera above the mount's feet
constexpr f32 kCameraBack = 0.5f;     // slight pull-back over the shoulder
constexpr f32 kDismountSide = 1.2f;   // lateral respawn offset
constexpr f32 kRideAccel = 8.0f;      // velocity smoothing rate (1/s)
constexpr f32 kTurnRate = 8.0f;       // mount facing smoothing (1/s)

} // namespace

void RideController::mount(ecs::Entity mount, f32 mountSpeed) {
    mount_ = mount;
    mountSpeed_ = glm::max(mountSpeed, 0.1f);
    velocity = Vec3 { 0.0f };
    justMounted_ = true;
    if (mount_.is_alive() && mount_.has<world::Transform>()) {
        // Start facing where the mount already faces (rotation * +Z =
        // {sin yaw, 0, -cos yaw}, the NPC convention).
        const Quat rotation = mount_.get<world::Transform>().rotation;
        const Vec3 forward = rotation * Vec3 { 0.0f, 0.0f, 1.0f };
        mountYaw_ = std::atan2(forward.x, -forward.z);
    }
}

void RideController::reset() {
    mount_ = ecs::Entity {};
    velocity = Vec3 { 0.0f };
}

void RideController::dismount(const RideContext& ctx) {
    Vec3 feet = ctx.flyCamera.camera.position;
    if (mount_.is_alive() && mount_.has<world::Transform>()) {
        const Vec3 at = mount_.get<world::Transform>().position;
        const f32 yaw = ctx.flyCamera.camera.yaw;
        const Vec3 right { std::cos(yaw), 0.0f, std::sin(yaw) };
        feet = at + right * kDismountSide;
    }
    // The enterPlayMode grounding idiom: feet on the height function,
    // +0.5 m so a slope never pins the spawn into the field.
    feet.y = render::terrain::height(ctx.terrainParams, feet.x, feet.z) +
             0.5f;
    reset();
    if (ctx.spawnPlayerBody) {
        ctx.spawnPlayerBody(feet);
    }
}

void RideController::update(f32 dt, const RideContext& ctx) {
    if (!mount_.is_alive() || !mount_.has<world::Transform>()) {
        // The mount died under us (cell streamed out): bail out to a
        // grounded capsule where the camera is.
        dismount(ctx);
        return;
    }
    // [E] again = dismount. The interaction scan (which mounted us) runs
    // EARLIER in the same frame and pressed() is a per-frame edge both
    // readers see — so the mount frame itself must not poll, or [E]
    // would mount and instantly dismount in one press.
    if (justMounted_) {
        justMounted_ = false;
    } else if (ctx.actions &&
               ctx.actions->pressed(ctx.input, InputAction::Interact)) {
        dismount(ctx);
        return;
    }

    // The shared mouselook (applyLookInput — same feel as on foot).
    render::FlyCamera& flyCamera = ctx.flyCamera;
    applyLookInput(flyCamera, ctx.input, ctx.settings, dt);

    // Camera-relative wish, flattened — the same input surface as on
    // foot (platform::moveAxis: WASD + left stick).
    const f32 yaw = flyCamera.camera.yaw;
    const Vec3 forward { std::sin(yaw), 0.0f, -std::cos(yaw) };
    const Vec3 right { std::cos(yaw), 0.0f, std::sin(yaw) };
    const Vec2 axis = platform::moveAxis(ctx.input);
    const Vec3 wish = forward * axis.y + right * axis.x;

    // The pure kinematic step (gameplay/actors/Riding — doctested):
    // smooth toward wish * mountSpeed, hug the terrain.
    auto& transform = mount_.get_mut<world::Transform>();
    const gameplay::RideState stepped = gameplay::stepRide(
        { transform.position, velocity }, wish, mountSpeed_, kRideAccel,
        dt, [&](f32 x, f32 z) {
            return render::terrain::height(ctx.terrainParams, x, z);
        });
    transform.position = stepped.position;
    velocity = stepped.velocity;

    // The mount turns toward its travel direction (shortest arc, smoothed
    // — a pony leans into the turn instead of snapping).
    if (glm::length(Vec2 { velocity.x, velocity.z }) > 0.5f) {
        const f32 heading = std::atan2(velocity.x, -velocity.z);
        mountYaw_ += wrapAngle(heading - mountYaw_) *
                     glm::min(1.0f, dt * kTurnRate);
        mountYaw_ = wrapAngle(mountYaw_);
    }
    transform.rotation = glm::angleAxis(glm::pi<f32>() - mountYaw_,
                                        Vec3 { 0.0f, 1.0f, 0.0f });

    // The player entity rides the saddle — followers, nameplate anchors
    // and the trigger sweep keep tracking the party leader; it FACES
    // where the camera looks (the PlayerController convention).
    if (ctx.playerEntity.is_alive() &&
        ctx.playerEntity.has<world::Transform>()) {
        auto& player = ctx.playerEntity.get_mut<world::Transform>();
        player.position =
            transform.position + Vec3 { 0.0f, kSaddleHeight, 0.0f };
        player.rotation = glm::angleAxis(glm::pi<f32>() - yaw,
                                         Vec3 { 0.0f, 1.0f, 0.0f });
    }

    // First-person from the saddle: above the withers, slightly back.
    flyCamera.camera.position = transform.position +
                                Vec3 { 0.0f, kRideEyeHeight, 0.0f } -
                                forward * kCameraBack;
}

} // namespace game
