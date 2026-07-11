#include "game/scenes/PlayerController.hpp"

#include <cmath>

#include <glm/glm.hpp>

#include "data/forms/CoreForms.hpp" // data::WeaponForm
#include "data/forms/FormDatabase.hpp"
#include "data/forms/LocForms.hpp"
#include "engine/core/Log.hpp"
#include "engine/physics/Physics.hpp"
#include "engine/platform/Input.hpp"
#include "engine/render/FlyCamera.hpp"
#include "game/scenes/InteractionController.hpp"
#include "game/scenes/NpcDirector.hpp" // Npc
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/event/EventBus.hpp"
#include "gameplay/actors/ActorState.hpp" // gameplay::Bounty
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/EquipmentStats.hpp"
#include "world/scene/Components.hpp"

namespace game {

namespace {

// B5.5: the stat-space -> world mapping and the movement feel now live in
// StatsTuningForm (audit U4-7, §5 moddable) — docs/STATS.md §3. Default
// sheet (~102): jog ~5.1 m/s, sprint x1.6 ~8.2 m/s, settles in ~0.1 s.
// (Dev feel pass 2026-07-06: +50% — the unencumbered adventurer is brisk;
// encumbrance will pull it back down when the P1 utility pass lands.)

} // namespace

void PlayerController::spawnBody(phys::PhysicsWorld& physics,
                                 const Vec3& position) {
    // [cpp-tuning] capsule radius/height (collision shape, not feel).
    body_ = std::make_unique<phys::CharacterBody>(physics, 0.3f, 1.8f,
                                                  position);
    velocity = Vec3 { 0.0f };
}

void PlayerController::destroyBody() {
    body_.reset();
}

// Chantier 3 B6: first-person melee — LMB swings the equipped weapon at
// the nearest living NPC in reach and roughly in front. Damage flows
// through the SAME GAS pipeline as the 2D arena (§2.9: no hand-rolled
// numbers). v1 has no swing animation (no visible body) — the cooldown
// and the hit feedback carry the feel until the FX/audio brick.
void PlayerController::tryAttack(const PlayerContext& ctx) {
    if (!ctx.playerEntity.is_alive() || !body_) {
        return;
    }
    // Chantier 4 B3: the swing uses the EQUIPPED weapon (inventory screen
    // can swap/unequip it); bare hands don't attack in v1.
    const data::WeaponForm* weapon = ctx.fallbackWeapon;
    if (ctx.playerEntity.has<gameplay::Equipment>()) {
        const auto& equipment = ctx.playerEntity.get<gameplay::Equipment>();
        weapon = equipment.weapon.isValid()
                     ? ctx.forms.find<data::WeaponForm>(equipment.weapon)
                     : nullptr;
    }
    if (!weapon) {
        LOG_INFO("Swing: no weapon equipped");
        return;
    }
    attackCooldown = 0.7f; // [cpp-tuning] melee swing cadence
    const Vec3 eye =
        body_->position() + Vec3 { 0.0f, ctx.statsTuning.eyeHeight, 0.0f };
    const Vec3 forward = ctx.flyCamera.camera.forward();
    Npc* best = nullptr;
    f32 bestScore = 0.45f;
    for (auto& npcPtr : ctx.npcs) {
        Npc& npc = *npcPtr;
        if (npc.dead || !npc.entity.is_alive()) {
            continue;
        }
        const Vec3 position =
            npc.entity.get<world::Transform>().position;
        const Vec3 to = position + Vec3 { 0.0f, 1.1f, 0.0f } - eye;
        const f32 distance = glm::length(to);
        if (distance > 2.4f || distance < 1e-3f) {
            continue;
        }
        const f32 facing = glm::dot(to / distance, forward);
        if (facing > bestScore) {
            bestScore = facing;
            best = &npc;
        }
    }
    if (!best) {
        LOG_INFO("Swing: nothing in reach");
        return;
    }
    gameplay::StatBlock block {
        best->entity.get_mut<gameplay::CoreAttributes>(),
        best->entity.get_mut<gameplay::AttributeSet>(),
        best->entity.get_mut<gameplay::AbilitySystem>(),
        best->entity.get_mut<gameplay::CombatState>()
    };
    const auto& playerSys = ctx.playerEntity.get<gameplay::AbilitySystem>();
    gameplay::DamageEvent event =
        gameplay::weaponDamageEvent(*weapon, playerSys);
    // C1: a target in its critical window eats the critical execution.
    if (const auto weakness = ctx.gameTags.find("State.CriticalWeakness")) {
        event.critical = block.system.tags.has(*weakness);
    }
    const gameplay::DamageResult result = gameplay::applyDamage(
        block, event, ctx.gameTags, ctx.derivedStats, nullptr,
        ctx.statsTuning);
    LOG_INFO("You hit for {:.0f} damage{}{} (target health {:.0f})",
             result.healthDamage, event.critical ? " — CRITICAL!" : "",
             result.staggered ? " — staggered!" : "",
             gameplay::currentValueOf(
                 best->entity.get<gameplay::AbilitySystem>(),
                 gameplay::attr("health")));

    // D2 — crime v1: assaulting a peaceful NPC in front of a witness.
    // Witnesses = the victim (if still alive) or any living NPC within
    // earshot with a clear line to the player (the B5 raycast idiom).
    if (!best->hostile) {
        const f32 witnessRange = ctx.statsTuning.crimeWitnessRange; // U4-7
        bool witnessed = !best->dead && best->entity.is_alive();
        for (const auto& witnessPtr : ctx.npcs) {
            if (witnessed) {
                break;
            }
            const Npc& witness = *witnessPtr;
            if (&witness == best || witness.dead ||
                !witness.entity.is_alive()) {
                continue;
            }
            const Vec3 witnessEye =
                witness.entity.get<world::Transform>().position +
                Vec3 { 0.0f, 1.5f, 0.0f };
            const Vec3 toPlayer = eye - witnessEye;
            const f32 sight = glm::length(toPlayer);
            if (sight > witnessRange || sight < 1e-3f) {
                continue;
            }
            const phys::RayHit hit =
                ctx.physics->rayCast(witnessEye, toPlayer / sight, sight);
            witnessed = !(hit.hit && hit.distance < sight - 0.6f);
        }
        if (witnessed && ctx.playerEntity.is_alive()) {
            auto& bounty = ctx.playerEntity.get_mut<gameplay::Bounty>();
            bounty.bounty += ctx.statsTuning.crimeBountyAssault; // U4-7
            ctx.syncWantedTag();
            ctx.interaction.say(
                ctx.texts.format(
                    "crime.observed",
                    std::to_string(static_cast<i32>(bounty.bounty))),
                4.0f);
            LOG_INFO("Crime witnessed — bounty {:.0f}", bounty.bounty);
        }
    }
}

void PlayerController::update(f32 dt, const PlayerContext& ctx) {
    if (!body_ || ctx.interaction.fading()) {
        return; // frozen during door transitions
    }
    platform::Input& input = ctx.input;
    // B6: melee swing on LMB (the mouse is captured in Play — ImGui
    // never owns it here).
    attackCooldown -= dt;
    if (attackCooldown <= 0.0f &&
        input.mousePressed(platform::MouseButton::Left)) {
        tryAttack(ctx);
    }

    // Mouselook, always captured in Play (no LMB gymnastics in a game).
    render::FlyCamera& flyCamera = ctx.flyCamera;
    const Vec2 look = input.mouseDelta();
    flyCamera.camera.yaw += look.x * flyCamera.lookSensitivity;
    flyCamera.camera.pitch = glm::clamp(
        flyCamera.camera.pitch - look.y * flyCamera.lookSensitivity,
        glm::radians(-89.0f), glm::radians(89.0f));

    // Camera-relative intent, flattened to the horizontal plane (§ the
    // controller OWNS motion — anims stay in place).
    const f32 yaw = flyCamera.camera.yaw;
    const Vec3 forward { std::sin(yaw), 0.0f, -std::cos(yaw) };
    const Vec3 right { std::cos(yaw), 0.0f, std::sin(yaw) };
    const Vec2 axis = platform::moveAxis(input); // U5-8
    const Vec3 wish = forward * axis.y + right * axis.x;
    const bool moving = glm::dot(wish, wish) > 0.0f;

    // B5.5: speeds come from the CURRENT derived stats (docs/STATS.md §3
    // — stat-space ~100 = nominal; injuries/buffs move them live). The
    // controller only READS attributes (§2.9); sprint pays energy through
    // the SprintCost effect below. Fallback keeps the scene alive without
    // a Player actor.
    const gameplay::StatsTuningForm& tuning = ctx.statsTuning; // U4-7
    f32 jog = 100.0f * tuning.movementSpeedScale3D;
    f32 accelRate = 100.0f * tuning.accelerationRate3D;
    f32 energy = 100.0f;
    if (ctx.playerEntity.is_alive()) {
        const auto& sys = ctx.playerEntity.get<gameplay::AbilitySystem>();
        jog = gameplay::currentValueOf(sys, gameplay::attr("movementSpeed")) *
              tuning.movementSpeedScale3D;
        accelRate =
            gameplay::currentValueOf(sys, gameplay::attr("acceleration")) *
            tuning.accelerationRate3D;
        energy = gameplay::currentValueOf(sys, gameplay::attr("energy"));
    }
    // C3: overencumbered = no sprint, no jump (STATS.md §3 Utility).
    const bool sprinting = moving && input.isDown(platform::Key::Shift) &&
                           energy > 1.0f && !ctx.overencumbered;
    const f32 targetSpeed =
        sprinting ? jog * tuning.sprintMultiplier : jog;
    const Vec3 target =
        moving ? glm::normalize(wish) * targetSpeed : Vec3 { 0.0f };
    // Exponential smoothing toward the target: snappy, never binary.
    velocity += (target - velocity) * (1.0f - std::exp(-accelRate * dt));
    if (input.wasPressed(platform::Key::Space) && !ctx.overencumbered) {
        // C3: jump velocity from the jumpPower stat (default sheet 104
        // → the previous hand-tuned 5.0 m/s via jumpPowerScale3D).
        f32 jump = jumpSpeed; // fallback without a Player actor
        if (ctx.playerEntity.is_alive()) {
            jump = gameplay::currentValueOf(
                       ctx.playerEntity.get<gameplay::AbilitySystem>(),
                       gameplay::attr("jumpPower")) *
                   tuning.jumpPowerScale3D;
        }
        body_->jump(jump);
    }
    body_->move(velocity, dt);

    // Chantier P0 C4a: the first-person player has no walk clip, so the
    // footstep AnimEvent is synthesized every strideLength meters of
    // grounded travel (NPCs get theirs from their clips' events).
    if (ctx.eventBus && body_->onGround()) {
        strideAccumulator +=
            glm::length(Vec2 { velocity.x, velocity.z }) * dt;
        const f32 stride = glm::max(tuning.strideLength, 0.5f);
        while (strideAccumulator >= stride) {
            strideAccumulator -= stride;
            gameplay::Event step;
            step.kind = gameplay::eventKind("AnimEvent");
            step.source = ctx.playerEntity;
            step.name = "Footstep";
            ctx.eventBus->dispatch(step);
        }
    } else if (!body_->onGround()) {
        strideAccumulator = 0.0f; // airborne: restart the stride
    }

    // Sprint cost: one instant GameplayEffect per half second (§2.9 — the
    // ONLY way energy moves; the spend also pauses regen for a beat).
    if (sprinting && ctx.sprintCostEffect && ctx.playerEntity.is_alive()) {
        sprintCostAccumulator += dt;
        while (sprintCostAccumulator >= 0.5f) {
            sprintCostAccumulator -= 0.5f;
            auto& set = ctx.playerEntity.get_mut<gameplay::AttributeSet>();
            auto& sys = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
            gameplay::applyEffect(set, sys, *ctx.sprintCostEffect,
                                  ctx.gameTags);
        }
    } else {
        sprintCostAccumulator = 0.0f;
    }

    // Eyes above the feet (eyeHeight, §5 U4-7); the ENTITY transform tracks
    // the capsule (the sim's view — extract/saves read this, not Jolt).
    flyCamera.camera.position =
        body_->position() + Vec3 { 0.0f, ctx.statsTuning.eyeHeight, 0.0f };
    if (ctx.playerEntity.is_alive()) {
        ctx.playerEntity.get_mut<world::Transform>().position =
            body_->position();
    }
}

} // namespace game
