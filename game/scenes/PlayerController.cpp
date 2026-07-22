#include "game/scenes/PlayerController.hpp"

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include "data/forms/CoreForms.hpp" // data::WeaponForm
#include "data/forms/FormDatabase.hpp"
#include "data/forms/LocForms.hpp"
#include "engine/core/Log.hpp"
#include "engine/physics/Physics.hpp"
#include "engine/platform/Input.hpp"
#include "engine/render/FlyCamera.hpp"
#include "game/scenes/InteractionController.hpp"
#include "game/scenes/LineOfSight.hpp" // hasLineOfSight
#include "game/scenes/NpcDirector.hpp" // Npc
#include "game/scenes/ProjectileDirector.hpp" // the bow
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayAbility.hpp" // tryActivate
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/combat/MeleeStrike.hpp"      // the ONE strike resolution
#include "gameplay/combat/MeleeSwing.hpp"       // the blade-touch swing
#include "gameplay/cue/GameplayCues.hpp"        // Cue.Hit/Block/Parry (C2)
#include "world/ai/Perception.hpp"              // sneak attack: unaware gate
#include "gameplay/event/EventBus.hpp"
#include "gameplay/actors/ActorState.hpp" // gameplay::Bounty
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/EquipmentStats.hpp"
#include "world/scene/Components.hpp"

namespace game {

namespace {

// The stat-space -> world mapping and the movement feel now live in
// StatsTuningForm (§5 moddable) — docs/STATS.md §3. Default
// sheet (~102): jog ~5.1 m/s, sprint x1.6 ~8.2 m/s, settles in ~0.1 s.
// (The unencumbered adventurer is deliberately brisk; encumbrance
// will pull it back down when the utility pass lands.)

// Skills-by-use: the player's activations opt into the usage-event channel
// (OnAbilityUsed -> skill XP + open quest vocabulary).
gameplay::AbilityContext playerAbilityContext(const PlayerContext& ctx) {
    gameplay::AbilityContext ability { ctx.forms, ctx.gameTags };
    ability.events = ctx.eventBus;
    ability.caster = ctx.playerEntity;
    return ability;
}

} // namespace

void PlayerController::spawnBody(phys::PhysicsWorld& physics,
                                 const Vec3& position) {
    // [cpp-tuning] capsule radius/height (collision shape, not feel).
    body_ = std::make_unique<phys::CharacterBody>(physics, 0.3f, 1.8f,
                                                  position);
    velocity = Vec3 { 0.0f };
    dodgeTimer = 0.0f;
    shiftHeldSeconds = 0.0f;
    moveMode_ = gameplay::MoveMode::Ground; // a fresh body spawns dry
}

void PlayerController::destroyBody() {
    body_.reset();
}

// LMB starts an ability-gated
// MeleeSwing — energy cost and cooldown are the AbilityForm's effects
// (§6), the swing phases are the weapon's data timings, and damage lands
// in updateSwing only where the VISIBLE blade passes (the
// blade must touch).
const data::WeaponForm* PlayerController::equippedWeapon(
    const PlayerContext& ctx) const {
    // The EQUIPPED weapon (the inventory screen swaps it);
    // the context fallback covers a bagless bootstrap.
    const data::WeaponForm* weapon = ctx.fallbackWeapon;
    if (ctx.playerEntity.is_alive() &&
        ctx.playerEntity.has<gameplay::Equipment>()) {
        const auto& equipment = ctx.playerEntity.get<gameplay::Equipment>();
        weapon = equipment.weapon.isValid()
                     ? ctx.forms.find<data::WeaponForm>(equipment.weapon)
                     : nullptr;
    }
    return weapon;
}

void PlayerController::tryAttack(const PlayerContext& ctx) {
    if (!ctx.playerEntity.is_alive() || !body_) {
        return;
    }
    // The first press on a holstered weapon DRAWS instead — that
    // path now lives in updateStance (the one weaponDrawn_ writer); the
    // dispatch never sends a sheathed press here.
    const data::WeaponForm* weapon = equippedWeapon(ctx);
    if (!weapon) {
        LOG_INFO("Swing: no weapon equipped");
        return;
    }
    auto& swing = ctx.playerEntity.get_mut<gameplay::MeleeSwing>();
    if (swing.phase != gameplay::SwingPhase::Idle) {
        return; // one swing in flight
    }
    if (ctx.attackAbility) {
        auto& set = ctx.playerEntity.get_mut<gameplay::AttributeSet>();
        auto& system = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
        if (!gameplay::tryActivate(*ctx.attackAbility, set, system, set,
                                   system, playerAbilityContext(ctx))) {
            return; // on cooldown or exhausted
        }
    }
    swingWeapon_ = weapon;
    gameplay::startSwing(swing);
}

void PlayerController::updateBowDraw(f32 dt, const PlayerContext& ctx,
                                     const data::WeaponForm& weapon,
                                     bool inhibited) {
    const gameplay::StatsTuningForm& tuning = ctx.statsTuning;
    const auto release = [&](bool fire) {
        const f32 charge = bowCharge_;
        bowCharge_ = -1.0f;
        bowDrawAccumulator = 0.0f;
        if (!fire || charge < 0.0f || !ctx.playerEntity.is_alive() ||
            !ctx.projectiles) {
            return;
        }
        // Ability gates at RELEASE (cost + cooldown, §6).
        if (ctx.attackAbility) {
            auto& set = ctx.playerEntity.get_mut<gameplay::AttributeSet>();
            auto& system =
                ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
            if (!gameplay::tryActivate(*ctx.attackAbility, set, system,
                                       set, system,
                                       playerAbilityContext(ctx))) {
                return;
            }
        }
        // The force is the DRAW: a tap looses a weak lob, a full draw
        // flies at the weapon's speed.
        const f32 factor =
            glm::mix(tuning.bowMinChargeFactor, 1.0f,
                     glm::clamp(charge, 0.0f, 1.0f));
        const render::Camera3D& cam = ctx.flyCamera.camera;
        gameplay::Projectile arrow;
        arrow.position = cam.position + cam.forward() * 0.6f;
        arrow.velocity = cam.forward() * weapon.projectileSpeed * factor;
        arrow.shooter = ctx.playerEntity.id();
        arrow.payload = gameplay::weaponDamageEvent(
            weapon, ctx.playerEntity.get<gameplay::AbilitySystem>());
        arrow.ammoItem = weapon.ammo; // recoverable once planted
        ctx.projectiles->spawn(arrow);
        if (weapon.ammo.isValid() &&
            ctx.playerEntity.has<gameplay::Inventory>()) {
            gameplay::removeItem(
                ctx.playerEntity.get_mut<gameplay::Inventory>(),
                weapon.ammo, 1);
        }
    };

    if (bowCharge_ < 0.0f) {
        // A sheathed bow: the first press unsheathes (the melee idiom) —
        // handled in updateStance (the one weaponDrawn_ writer), which
        // skips this call on the frame it draws.
        // Not drawing: LMB starts the draw (never inhibited, sheathed,
        // mid-swing, or with a dry quiver — refused before any cost).
        if (inhibited || !weaponDrawn_ ||
            !ctx.actions->pressed(ctx.input, InputAction::Attack) ||
            !ctx.playerEntity.is_alive() ||
            ctx.playerEntity.get<gameplay::MeleeSwing>().phase !=
                gameplay::SwingPhase::Idle) {
            return;
        }
        if (weapon.ammo.isValid() &&
            ctx.playerEntity.has<gameplay::Inventory>() &&
            gameplay::itemCount(
                ctx.playerEntity.get<gameplay::Inventory>(),
                weapon.ammo) <= 0) {
            LOG_INFO("A7: out of arrows");
            return;
        }
        bowCharge_ = 0.0f;
        return;
    }
    // Drawing. A stagger breaks the draw — the arrow stays nocked.
    if (inhibited) {
        release(false);
        return;
    }
    bowCharge_ = glm::min(
        1.0f, bowCharge_ + dt / glm::max(tuning.bowDrawSeconds, 0.05f));
    // Holding the draw is effortful: the BowDrawCost effect ticks the
    // usual half-second accumulator (§2.9 — only effects move energy).
    if (ctx.bowDrawCostEffect && ctx.playerEntity.is_alive()) {
        gameplay::tickPeriodicEffect(
            bowDrawAccumulator, dt, 0.5f,
            ctx.playerEntity.get_mut<gameplay::AttributeSet>(),
            ctx.playerEntity.get_mut<gameplay::AbilitySystem>(),
            *ctx.bowDrawCostEffect, ctx.gameTags);
    }
    // Exhausted arms give in — the arrow flies at the current draw.
    bool exhausted = false;
    if (ctx.playerEntity.is_alive()) {
        if (const auto tag = ctx.gameTags.find("State.Exhausted")) {
            exhausted = ctx.playerEntity.get<gameplay::AbilitySystem>()
                            .tags.has(*tag);
        }
    }
    if (!ctx.actions->down(ctx.input, InputAction::Attack) || exhausted) {
        release(true);
    }
}

// The swing machine + the blade-touch hit test. The blade segment
// (grip -> +Y x bladeLength x hitTolerance — the sword mesh grows along
// +Y) rides the simulated camera socket through the Active sweep and is
// tested ANALYTICALLY against the NPC capsules: CharacterVirtual bodies
// are outside the Jolt broadphase, no physics cast can do this.
void PlayerController::updateSwing(f32 dt, const PlayerContext& ctx) {
    if (!ctx.playerEntity.is_alive()) {
        return;
    }
    auto& swing = ctx.playerEntity.get_mut<gameplay::MeleeSwing>();
    if (swing.phase == gameplay::SwingPhase::Idle || !swingWeapon_) {
        swingWeapon_ = nullptr;
        return;
    }
    const gameplay::SwingTiming timing { swingWeapon_->swingWindup,
                                         swingWeapon_->swingActive,
                                         swingWeapon_->swingRecovery };
    gameplay::updateSwing(swing, dt, timing);
    if (swing.phase == gameplay::SwingPhase::Active) {
        const render::Camera3D& cam = ctx.flyCamera.camera;
        const Vec3 fwd = cam.forward();
        const Vec3 right = cam.right();
        const Vec3 up = glm::normalize(glm::cross(right, fwd));
        const Mat4 basis { Vec4 { right, 0.0f }, Vec4 { up, 0.0f },
                           Vec4 { -fwd, 0.0f },
                           Vec4 { cam.position, 1.0f } };
        const Mat4 socket =
            basis * gameplay::swingSocketLocal(swing, timing);
        const Vec3 grip { socket[3] };
        const Vec3 bladeDir = glm::normalize(Vec3 { socket[1] });
        const Vec3 tip = grip + bladeDir * (swingWeapon_->bladeLength *
                                            swingWeapon_->hitTolerance);
        for (auto& npcPtr : ctx.npcs) {
            Npc& npc = *npcPtr;
            // A DOWNED ally is no target — the blade passes (revive
            // him with [E] instead; aggravation is the bleedout's job).
            if (npc.dead || npc.downed || !npc.entity.is_alive()) {
                continue;
            }
            const Vec3 feet = npc.entity.get<world::Transform>().position;
            if (!gameplay::segmentHitsActor(grip, tip, feet)) {
                continue;
            }
            if (gameplay::registerStrike(swing, npc.entity.id())) {
                applyHit(ctx, npc, *swingWeapon_);
            }
        }
    }
    if (swing.phase == gameplay::SwingPhase::Idle) {
        swingWeapon_ = nullptr; // swing completed this frame
    }
}

// The weapon hit: typed damage through the GAS
// pipeline (§2.9) + the D2 crime pass — now fired by blade CONTACT. The
// exchange rules (crit window, guard cone, perfect parry, events, cues)
// live in ONE place, resolveMeleeStrike, shared with the NPC side.
void PlayerController::applyHit(const PlayerContext& ctx, Npc& target,
                                const data::WeaponForm& weapon) {
    const Vec3 eye =
        body_->position() + Vec3 { 0.0f, ctx.statsTuning.eyeHeight, 0.0f };
    gameplay::StatBlock defender {
        target.entity.get_mut<gameplay::CoreAttributes>(),
        target.entity.get_mut<gameplay::AttributeSet>(),
        target.entity.get_mut<gameplay::AbilitySystem>(),
        target.entity.get_mut<gameplay::CombatState>()
    };
    gameplay::StatBlock attacker {
        ctx.playerEntity.get_mut<gameplay::CoreAttributes>(),
        ctx.playerEntity.get_mut<gameplay::AttributeSet>(),
        ctx.playerEntity.get_mut<gameplay::AbilitySystem>(),
        ctx.playerEntity.get_mut<gameplay::CombatState>()
    };
    const auto& targetT = target.entity.get<world::Transform>();
    gameplay::StrikeGeometry geo {
        body_->position(), targetT.position,
        targetT.rotation * Vec3 { 0.0f, 0.0f, 1.0f },
        target.entity.get<gameplay::MeleeSwing>().guardSeconds,
        targetT.position + Vec3 { 0.0f, 1.2f, 0.0f }
    };
    // Sneak attack: the flat seam — the defender's world-layer Perception
    // is read HERE (Calm = he never noticed you); the strike rules only
    // ever see the bool + the attacker's State.Sneaking tag.
    if (target.entity.has<world::Perception>()) {
        geo.targetUnaware =
            world::awareState(target.entity.get<world::Perception>()) ==
            world::AwareState::Calm;
    }
    const gameplay::StrikeContext strike { ctx.gameTags, ctx.derivedStats,
                                           ctx.statsTuning, ctx.eventBus,
                                           ctx.cues };
    const gameplay::StrikeOutcome outcome = gameplay::resolveMeleeStrike(
        attacker, defender, ctx.playerEntity, target.entity,
        gameplay::weaponDamageEvent(weapon, attacker.system), geo, strike);
    if (outcome.sneakAttack) {
        LOG_INFO("SNEAK ATTACK — x{:.1f}",
                 ctx.statsTuning.sneakAttackMultiplier);
    }
    if (outcome.guard.perfect) {
        LOG_INFO("PERFECT PARRY — your poise takes {}",
                 ctx.statsTuning.perfectParryPosture);
    } else {
        if (outcome.guard.caught) {
            LOG_INFO("Blocked!");
        }
        LOG_INFO("You hit for {:.0f} damage{}{} (target health {:.0f})",
                 outcome.damage.healthDamage,
                 outcome.critical ? " — CRITICAL!" : "",
                 outcome.damage.staggered ? " — staggered!" : "",
                 gameplay::currentValueOf(
                     target.entity.get<gameplay::AbilitySystem>(),
                     gameplay::attr("health")));
    }

    // The strike resolved; the crime pass is its own concern (review C2:
    // applyHit used to weld the two together).
    if (!target.hostile) {
        witnessCrime(ctx, target, eye);
    }
}

// D2 — crime v1: assaulting a peaceful NPC in front of a witness.
// Witnesses = the victim (if still alive) or any living NPC within
// earshot with a clear line to the player (the LOS raycast idiom).
void PlayerController::witnessCrime(const PlayerContext& ctx,
                                    const Npc& target,
                                    const Vec3& playerEye) {
    const f32 witnessRange = ctx.statsTuning.crimeWitnessRange;
    bool witnessed = !target.dead && target.entity.is_alive();
    // Per-faction crime: the bounty goes to the WITNESS's
    // faction — the victim's if it saw its own assault, else the
    // bystander's. A factionless witness raises the unattributed total
    // (every guard reacts — the pre-migration behavior).
    gameplay::GameplayTag witnessFaction =
        witnessed ? target.factionTag : gameplay::GameplayTag {};
    for (const auto& witnessPtr : ctx.npcs) {
        if (witnessed) {
            break;
        }
        const Npc& witness = *witnessPtr;
        if (&witness == &target || witness.dead ||
            !witness.entity.is_alive()) {
            continue;
        }
        const Vec3 witnessEye =
            witness.entity.get<world::Transform>().position +
            Vec3 { 0.0f, 1.5f, 0.0f };
        const f32 range = glm::length(playerEye - witnessEye);
        if (range > witnessRange || range < 1e-3f) {
            continue;
        }
        witnessed = hasLineOfSight(*ctx.physics, witnessEye, playerEye);
        if (witnessed) {
            witnessFaction = witness.factionTag;
        }
    }
    if (witnessed && ctx.playerEntity.is_alive()) {
        auto& bounty = ctx.playerEntity.get_mut<gameplay::Bounty>();
        gameplay::addBounty(bounty, witnessFaction,
                            ctx.statsTuning.crimeBountyAssault);
        ctx.syncWantedTag();
        ctx.interaction.say(
            ctx.texts.format(
                "crime.observed",
                std::to_string(static_cast<i32>(bounty.bounty))),
            4.0f);
        LOG_INFO("Crime witnessed — bounty {:.0f}", bounty.bounty);
    }
}

// The swim branch: decideMoveMode owns WHEN (sim-pure,
// doctested), this owns HOW. Full-3D wish toward the look, clamped so
// the head never breaches the surface from below; the SwimCost effect
// drains energy on the sprint-cost accumulator pattern (§2.9); an
// exhausted swimmer SINKS and drowns on a periodic unmitigated tick.
bool PlayerController::updateSwimming(f32 dt, const PlayerContext& ctx,
                                      f32 jog, f32 accelRate) {
    const gameplay::StatsTuningForm& tuning = ctx.statsTuning;
    const std::optional<f32> surface =
        ctx.waterSurfaceAt ? ctx.waterSurfaceAt(body_->position())
                           : std::nullopt;
    const gameplay::MoveMode next = gameplay::decideMoveMode(
        moveMode_, surface, body_->position().y, tuning.eyeHeight,
        body_->onGround(), tuning.swimSubmergeDepth,
        tuning.swimWadeOutRatio);
    if (next != moveMode_) {
        // THE transition: the facade follows the mode.
        moveMode_ = next;
        body_->setSwimming(moveMode_ == gameplay::MoveMode::Swim);
        swimCostAccumulator = 0.0f;
        drownAccumulator = 0.0f;
    }
    if (moveMode_ != gameplay::MoveMode::Swim) {
        return false;
    }
    // Water absorbs the landing: no fall damage out of a swim.
    wasGrounded_ = true;

    bool exhausted = false;
    if (ctx.playerEntity.is_alive()) {
        if (const auto tag = ctx.gameTags.find("State.Exhausted")) {
            exhausted = ctx.playerEntity.get<gameplay::AbilitySystem>()
                            .tags.has(*tag);
        }
    }
    // Swim toward the LOOK (pitch included); Space paddles up.
    const render::Camera3D& cam = ctx.flyCamera.camera;
    const Vec3 fwd3 = cam.forward();
    const Vec3 right = cam.right();
    const Vec2 axis = platform::moveAxis(ctx.input);
    Vec3 wish3 = fwd3 * axis.y + right * axis.x;
    if (ctx.actions->down(ctx.input, InputAction::Jump)) {
        wish3.y += 1.0f; // Space/A paddles up
    }
    const f32 swimSpeed = jog * tuning.swimSpeedFactor;
    Vec3 target = glm::dot(wish3, wish3) > 1e-6f
                      ? glm::normalize(wish3) * swimSpeed
                      : Vec3 { 0.0f };
    if (exhausted) {
        // No strength left: the water wins (STATS.md survival loop).
        target.y = glm::min(target.y, 0.0f) - tuning.swimExhaustedSink;
    }
    // Surface clamp: swimming never breaches — decideMoveMode handles
    // the actual exit (shallows, or the head clearing the surface).
    if (surface &&
        body_->position().y + tuning.eyeHeight > *surface - 0.05f &&
        target.y > 0.0f) {
        target.y = 0.0f;
    }
    velocity += (target - velocity) * (1.0f - std::exp(-accelRate * dt));
    body_->move(velocity, dt);

    // Energy drain: one instant effect per half second (§2.9 — the only
    // way energy moves), the sprint-cost pattern.
    if (ctx.swimCostEffect && ctx.playerEntity.is_alive() && !exhausted) {
        gameplay::tickPeriodicEffect(
            swimCostAccumulator, dt, 0.5f,
            ctx.playerEntity.get_mut<gameplay::AttributeSet>(),
            ctx.playerEntity.get_mut<gameplay::AbilitySystem>(),
            *ctx.swimCostEffect, ctx.gameTags);
    }
    // Drowning: exhausted underwater = an unmitigated tick per second
    // (resist penetration eats the target's own mitigation, never
    // amplifies) — death flows through the normal pipeline.
    if (exhausted && ctx.playerEntity.is_alive()) {
        drownAccumulator += dt;
        while (drownAccumulator >= 1.0f) {
            drownAccumulator -= 1.0f;
            gameplay::StatBlock block {
                ctx.playerEntity.get_mut<gameplay::CoreAttributes>(),
                ctx.playerEntity.get_mut<gameplay::AttributeSet>(),
                ctx.playerEntity.get_mut<gameplay::AbilitySystem>(),
                ctx.playerEntity.get_mut<gameplay::CombatState>()
            };
            gameplay::DamageEvent drown;
            drown.channels = { { gameplay::DamageType::Chemical,
                                 tuning.drownDamagePerSecond } };
            drown.resistPenetration = 1000.0f;
            gameplay::applyDamage(block, drown, ctx.gameTags,
                                  ctx.derivedStats, nullptr, tuning);
            LOG_INFO("D2b: drowning — {:.0f} damage",
                     tuning.drownDamagePerSecond);
        }
    } else {
        drownAccumulator = 0.0f;
    }
    return true;
}

void PlayerController::update(f32 dt, const PlayerContext& ctx) {
    if (!body_ || ctx.interaction.fading()) {
        return; // frozen during door transitions
    }
    // Two halves, in a fixed per-frame order —
    // stance (toggles, THE action decision, combat input dispatch) then
    // locomotion (look, move, dodge, drains, camera sync).
    const StanceFrame frame = updateStance(dt, ctx);
    updateLocomotion(dt, ctx, frame);
}

PlayerController::StanceFrame
PlayerController::updateStance(f32 dt, const PlayerContext& ctx) {
    platform::Input& input = ctx.input;
    StanceFrame frame;
    // RMB held = raised guard — the State.Blocking tag is the §6
    // vocabulary the damage paths read (both camps). Guarding excludes
    // swinging (and vice versa: the guard waits for the swing to land).
    // R: draw/sheathe — never mid-swing.
    if (ctx.actions->pressed(input, InputAction::DrawSheathe) &&
        (!ctx.playerEntity.is_alive() ||
         ctx.playerEntity.get<gameplay::MeleeSwing>().phase ==
             gameplay::SwingPhase::Idle)) {
        weaponDrawn_ = !weaponDrawn_;
    }
    // Ctrl: sneak toggle — the body CROUCHES to
    // half height (standing back up can be refused by a low ceiling),
    // steps soften, detection halves (State.Sneaking drives it all).
    if (ctx.actions->pressed(input, InputAction::Sneak)) {
        const bool want = !sneaking_;
        if (body_->setCrouched(want)) {
            sneaking_ = want;
            sneakCostAccumulator = 0.0f;
            LOG_INFO("Sneak {}", sneaking_ ? "ON" : "off");
        } else {
            LOG_INFO("Sneak: no room to stand up here");
        }
    }
    if (ctx.playerEntity.is_alive()) {
        gameplay::syncStateTag(
            ctx.playerEntity.get_mut<gameplay::AbilitySystem>(),
            ctx.gameTags, "State.Sneaking", sneaking_);
    }
    // STATS.md §4: staggered = can't act, parry or dodge, very slow.
    if (ctx.playerEntity.is_alive()) {
        if (const auto tag = ctx.gameTags.find("State.Staggered")) {
            frame.staggered =
                ctx.playerEntity.get<gameplay::AbilitySystem>().tags.has(
                    *tag);
        }
    }
    const data::WeaponForm* held = equippedWeapon(ctx);
    frame.rangedWeapon = held && held->projectileSpeed > 0.0f;
    // THE action decision — every exclusion (guard vs swing,
    // stagger, bow draw, dodge) lives in gameplay::decidePlayerAction;
    // nothing below re-derives one. A dead player stays Idle (the old
    // `blocking = false` path).
    if (ctx.playerEntity.is_alive()) {
        auto& system = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
        auto& swing = ctx.playerEntity.get_mut<gameplay::MeleeSwing>();
        gameplay::PlayerActionInputs in;
        in.weaponDrawn = weaponDrawn_;
        in.rangedWeapon = frame.rangedWeapon;
        in.staggered = frame.staggered;
        in.blockHeld = ctx.actions->down(input, InputAction::Block);
        in.swingInFlight = swing.phase != gameplay::SwingPhase::Idle;
        in.drawingBow = bowCharge_ >= 0.0f;
        frame.action = gameplay::decidePlayerAction(in);
        const bool blocking =
            frame.action == gameplay::PlayerAction::Blocking;
        // The guard clock: a hit landing inside the fresh window is a
        // PERFECT parry (applyBlock reads guardSeconds).
        gameplay::tickGuard(swing, blocking, dt);
        gameplay::syncStateTag(system, ctx.gameTags, "State.Blocking",
                               blocking);
    }
    // The first press on a holstered weapon DRAWS it and does nothing
    // else that frame (no swing, no charge) — folded here from
    // tryAttack/updateBowDraw so weaponDrawn_ has exactly ONE writer.
    // The melee path needs a live actor (the old tryAttack early-out);
    // the bow path never had that gate.
    bool drewThisFrame = false;
    if (!weaponDrawn_ && !frame.staggered &&
        ctx.actions->pressed(input, InputAction::Attack) &&
        (frame.rangedWeapon ? bowCharge_ < 0.0f
                            : ctx.playerEntity.is_alive())) {
        weaponDrawn_ = true;
        drewThisFrame = true;
    }
    // Melee swing on LMB (the mouse is captured in Play — ImGui
    // never owns it here). Cadence is the ability's cooldown effect plus
    // the swing itself: no hardcoded timer.
    if (frame.rangedWeapon) {
        // Ranged = the CHARGED shot (hold to draw, release to
        // loose); melee inputs stay out of the way. 7b: a bow raises no
        // guard, so only the stagger inhibits/cuts the draw.
        if (!drewThisFrame) {
            updateBowDraw(dt, ctx, *held, frame.staggered);
        }
    } else {
        if (bowCharge_ >= 0.0f) {
            bowCharge_ = -1.0f; // weapon swapped mid-draw: let it down
        }
        if (frame.action == gameplay::PlayerAction::Idle &&
            !frame.staggered && !drewThisFrame &&
            ctx.actions->pressed(input, InputAction::Attack)) {
            tryAttack(ctx);
        }
    }
    updateSwing(dt, ctx);
    return frame;
}

void PlayerController::updateLocomotion(f32 dt, const PlayerContext& ctx,
                                        const StanceFrame& frame) {
    platform::Input& input = ctx.input;
    const bool staggered = frame.staggered;
    const bool blocking =
        frame.action == gameplay::PlayerAction::Blocking;
    // Look, always captured in Play (no LMB gymnastics in a game).
    // Mouse (pixels x base sens x user multiplier) + right stick
    // (rad/s at full deflection x dt); one invert-Y switch covers both.
    render::FlyCamera& flyCamera = ctx.flyCamera;
    const f32 mouseSens =
        flyCamera.lookSensitivity *
        (ctx.settings ? ctx.settings->mouseSensitivity : 1.0f);
    Vec2 look = input.mouseDelta() * mouseSens; // radians; +y = look down
    if (ctx.settings) {
        const Vec2 stick = input.rightStick(); // +y = stick up = look up
        look.x += stick.x * ctx.settings->stickSensitivity * dt;
        look.y -= stick.y * ctx.settings->stickSensitivity * dt;
        if (ctx.settings->invertLookY) {
            look.y = -look.y;
        }
    }
    flyCamera.camera.yaw += look.x;
    flyCamera.camera.pitch = glm::clamp(
        flyCamera.camera.pitch - look.y,
        glm::radians(-89.0f), glm::radians(89.0f));

    // Camera-relative intent, flattened to the horizontal plane (§ the
    // controller OWNS motion — anims stay in place).
    const f32 yaw = flyCamera.camera.yaw;
    const Vec3 forward { std::sin(yaw), 0.0f, -std::cos(yaw) };
    const Vec3 right { std::cos(yaw), 0.0f, std::sin(yaw) };
    const Vec2 axis = platform::moveAxis(input);
    const Vec3 wish = forward * axis.y + right * axis.x;
    const bool moving = glm::dot(wish, wish) > 0.0f;

    // Speeds come from the CURRENT derived stats (docs/STATS.md §3
    // — stat-space ~100 = nominal; injuries/buffs move them live). The
    // controller only READS attributes (§2.9); sprint pays energy through
    // the SprintCost effect below. Fallback keeps the scene alive without
    // a Player actor.
    const gameplay::StatsTuningForm& tuning = ctx.statsTuning;
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
    // Swimming consumes the whole ground-movement section (no
    // jump/dodge/sprint/strides in the water); the camera/transform sync
    // at the tail still runs.
    if (updateSwimming(dt, ctx, jog, accelRate)) {
        flyCamera.camera.position =
            body_->position() +
            Vec3 { 0.0f, ctx.statsTuning.eyeHeight, 0.0f };
        if (ctx.playerEntity.is_alive()) {
            auto& transform =
                ctx.playerEntity.get_mut<world::Transform>();
            transform.position = body_->position();
            transform.rotation = glm::angleAxis(
                glm::pi<f32>() - yaw, Vec3 { 0.0f, 1.0f, 0.0f });
        }
        return;
    }
    // Dodge (the 2D arena move in 3D): a TAP on
    // the sprint key — released within dodgeTapSeconds — bursts in the
    // held move direction, backward when none. Cost, cooldown and the
    // State.Dodging i-frames are the Dodge ability's effects (§6).
    if (ctx.actions->down(input, InputAction::SprintDodge)) {
        shiftHeldSeconds += dt;
    } else {
        if (shiftHeldSeconds > 0.0f &&
            shiftHeldSeconds <= tuning.dodgeTapSeconds &&
            dodgeTimer <= 0.0f && ctx.playerEntity.is_alive()) {
            // The tap is only a REQUEST — the same arbiter that owns
            // every exclusion (stagger, swing in flight, guard, draw)
            // answers it; no guard is re-checked here. Swing state is
            // re-read: a swing may have STARTED in this frame's stance.
            gameplay::PlayerActionInputs in;
            in.weaponDrawn = weaponDrawn_;
            in.rangedWeapon = frame.rangedWeapon;
            in.staggered = staggered;
            in.blockHeld = ctx.actions->down(input, InputAction::Block);
            in.swingInFlight =
                ctx.playerEntity.get<gameplay::MeleeSwing>().phase !=
                gameplay::SwingPhase::Idle;
            in.drawingBow = bowCharge_ >= 0.0f;
            in.dodgeRequested = true;
            if (gameplay::decidePlayerAction(in) ==
                gameplay::PlayerAction::Dodging) {
                bool activated = true;
                if (ctx.dodgeAbility) {
                    auto& set =
                        ctx.playerEntity.get_mut<gameplay::AttributeSet>();
                    auto& system =
                        ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
                    activated = gameplay::tryActivate(
                        *ctx.dodgeAbility, set, system, set, system,
                        playerAbilityContext(ctx));
                }
                if (activated) {
                    dodgeDir = moving ? glm::normalize(wish) : -forward;
                    dodgeTimer = tuning.dodgeDurationSeconds;
                }
            }
        }
        shiftHeldSeconds = 0.0f;
    }

    // C3: overencumbered = no sprint, no jump (STATS.md §3 Utility).
    // No sprint behind a raised guard either.
    const bool sprinting = moving &&
                           ctx.actions->down(input,
                                             InputAction::SprintDodge) &&
                           energy > 1.0f && !ctx.overencumbered &&
                           !blocking;
    f32 targetSpeed = sprinting ? jog * tuning.sprintMultiplier : jog;
    if (blocking) {
        targetSpeed *= tuning.blockSpeedFactor; // guarding is careful
    }
    if (sneaking_) {
        targetSpeed *= tuning.sneakSpeedFactor; // crouched = deliberate
    }
    if (staggered) {
        targetSpeed *= tuning.staggerSpeedFactor; // §4: reeling is slow
    }
    const Vec3 target =
        moving ? glm::normalize(wish) * targetSpeed : Vec3 { 0.0f };
    // Exponential smoothing toward the target: snappy, never binary.
    velocity += (target - velocity) * (1.0f - std::exp(-accelRate * dt));
    // The dodge burst OVERRIDES the smoothed intent: crisp in, smooth
    // out (the smoothing above resumes from the burst velocity).
    if (dodgeTimer > 0.0f) {
        dodgeTimer -= dt;
        velocity = dodgeDir * (jog * tuning.dodgeSpeedMultiplier);
    }
    if (ctx.actions->pressed(input, InputAction::Jump) &&
        !ctx.overencumbered) {
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

    // Fall damage: track the airborne
    // peak, pay on the landing edge. Blunt and unmitigated (the drowning
    // idiom); a lethal height goes through killOutright (the kill-z
    // idiom) so death flows through the normal pipeline.
    const bool grounded = body_->onGround();
    const f32 feetY = body_->position().y;
    if (!grounded) {
        fallPeakY_ = wasGrounded_ ? feetY : glm::max(fallPeakY_, feetY);
    } else if (!wasGrounded_ && ctx.playerEntity.is_alive()) {
        const f32 fallHeight = fallPeakY_ - feetY;
        if (fallHeight >= tuning.fallMinHeight) {
            gameplay::StatBlock block {
                ctx.playerEntity.get_mut<gameplay::CoreAttributes>(),
                ctx.playerEntity.get_mut<gameplay::AttributeSet>(),
                ctx.playerEntity.get_mut<gameplay::AbilitySystem>(),
                ctx.playerEntity.get_mut<gameplay::CombatState>()
            };
            if (fallHeight >= tuning.fallLethalHeight) {
                gameplay::killOutright(block, ctx.gameTags,
                                       ctx.derivedStats, tuning);
                LOG_INFO("Fall: lethal landing after {:.1f} m", fallHeight);
            } else {
                gameplay::DamageEvent fall;
                fall.channels = { { gameplay::DamageType::Blunt,
                                    gameplay::fallDamage(fallHeight,
                                                         tuning) } };
                fall.armorPenetration = 1000.0f; // the ground ignores armor
                gameplay::applyDamage(block, fall, ctx.gameTags,
                                      ctx.derivedStats, nullptr, tuning);
                LOG_INFO("Fall: {:.1f} m — {:.0f} damage", fallHeight,
                         gameplay::fallDamage(fallHeight, tuning));
            }
        }
    }
    wasGrounded_ = grounded;

    // The first-person player has no walk clip, so the
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
        gameplay::tickPeriodicEffect(
            sprintCostAccumulator, dt, 0.5f,
            ctx.playerEntity.get_mut<gameplay::AttributeSet>(),
            ctx.playerEntity.get_mut<gameplay::AbilitySystem>(),
            *ctx.sprintCostEffect, ctx.gameTags);
    } else {
        sprintCostAccumulator = 0.0f;
    }
    // Sneak drain: MOVING sneaked pays (SneakCost, ~3/s, data); holding
    // still and watching is free.
    if (sneaking_ && moving && ctx.sneakCostEffect &&
        ctx.playerEntity.is_alive()) {
        gameplay::tickPeriodicEffect(
            sneakCostAccumulator, dt, 0.5f,
            ctx.playerEntity.get_mut<gameplay::AttributeSet>(),
            ctx.playerEntity.get_mut<gameplay::AbilitySystem>(),
            *ctx.sneakCostEffect, ctx.gameTags);
    } else if (!moving) {
        sneakCostAccumulator = 0.0f;
    }

    // Eyes above the feet (eyeHeight, §5-tunable; a crouch halves it —
    // give the sneaker a hair over the capsule's half height); the
    // ENTITY transform tracks the capsule (the sim's view).
    flyCamera.camera.position =
        body_->position() +
        Vec3 { 0.0f,
               ctx.statsTuning.eyeHeight * (sneaking_ ? 0.5f : 1.0f),
               0.0f };
    if (ctx.playerEntity.is_alive()) {
        auto& transform = ctx.playerEntity.get_mut<world::Transform>();
        transform.position = body_->position();
        // The entity FACES where the camera looks (rotation * +Z =
        // horizontal camera forward, the NPC yaw convention) — the guard
        // cone and any future sim consumer read this, never the camera.
        transform.rotation = glm::angleAxis(glm::pi<f32>() - yaw,
                                            Vec3 { 0.0f, 1.0f, 0.0f });
    }
}

} // namespace game
