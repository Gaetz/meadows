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
#include "game/scenes/NpcDirector.hpp" // Npc
#include "game/scenes/ProjectileDirector.hpp" // A7: the bow
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayAbility.hpp" // tryActivate (P0 A3)
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/combat/MeleeSwing.hpp"       // the blade-touch swing (A4)
#include "gameplay/cue/GameplayCues.hpp"        // Cue.Hit/Block/Parry (C2)
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
    dodgeTimer = 0.0f;
    shiftHeldSeconds = 0.0f;
    moveMode_ = gameplay::MoveMode::Ground; // a fresh body spawns dry
}

void PlayerController::destroyBody() {
    body_.reset();
}

// P0 A3 (replaces the B6 cone pick): LMB starts an ability-gated
// MeleeSwing — energy cost and cooldown are the AbilityForm's effects
// (§6), the swing phases are the weapon's data timings, and damage lands
// in updateSwing only where the VISIBLE blade passes (dev design: the
// blade must touch).
const data::WeaponForm* PlayerController::equippedWeapon(
    const PlayerContext& ctx) const {
    // Chantier 4 B3: the EQUIPPED weapon (the inventory screen swaps it);
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
    if (!weaponDrawn_) {
        weaponDrawn_ = true; // the first press draws; the next one swings
        return;
    }
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
                                   system,
                                   { ctx.forms, ctx.gameTags })) {
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
                                       { ctx.forms, ctx.gameTags })) {
                return;
            }
        }
        // The force is the DRAW: a tap looses a weak lob, a full draw
        // flies at the weapon's speed (dev design 2026-07-12).
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
        // A sheathed bow: the first press unsheathes (the melee idiom).
        if (!inhibited && !weaponDrawn_ &&
            ctx.input.mousePressed(platform::MouseButton::Left)) {
            weaponDrawn_ = true;
            return;
        }
        // Not drawing: LMB starts the draw (never inhibited, sheathed,
        // mid-swing, or with a dry quiver — refused before any cost).
        if (inhibited || !weaponDrawn_ ||
            !ctx.input.mousePressed(platform::MouseButton::Left) ||
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
        bowDrawAccumulator += dt;
        while (bowDrawAccumulator >= 0.5f) {
            bowDrawAccumulator -= 0.5f;
            auto& set = ctx.playerEntity.get_mut<gameplay::AttributeSet>();
            auto& sys = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
            gameplay::applyEffect(set, sys, *ctx.bowDrawCostEffect,
                                  ctx.gameTags);
        }
    }
    // Exhausted arms give in — the arrow flies at the current draw.
    bool exhausted = false;
    if (ctx.playerEntity.is_alive()) {
        if (const auto tag = ctx.gameTags.find("State.Exhausted")) {
            exhausted = ctx.playerEntity.get<gameplay::AbilitySystem>()
                            .tags.has(*tag);
        }
    }
    if (!ctx.input.mouseDown(platform::MouseButton::Left) || exhausted) {
        release(true);
    }
}

// P0 A4: the swing machine + the blade-touch hit test. The blade segment
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
            if (npc.dead || !npc.entity.is_alive()) {
                continue;
            }
            const Vec3 feet = npc.entity.get<world::Transform>().position;
            // [cpp-tuning] the shared humanoid capsule (feet-anchored).
            constexpr f32 kRadius = 0.4f;
            constexpr f32 kHeight = 1.8f;
            if (!gameplay::segmentHitsCapsule(
                    grip, tip, feet + Vec3 { 0.0f, kRadius, 0.0f },
                    feet + Vec3 { 0.0f, kHeight - kRadius, 0.0f },
                    kRadius)) {
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

// The B6 weapon hit, unchanged in substance: typed damage through the GAS
// pipeline (§2.9) + the D2 crime pass — now fired by blade CONTACT.
void PlayerController::applyHit(const PlayerContext& ctx, Npc& target,
                                const data::WeaponForm& weapon) {
    Npc* best = &target;
    const Vec3 eye =
        body_->position() + Vec3 { 0.0f, ctx.statsTuning.eyeHeight, 0.0f };
    gameplay::StatBlock block {
        best->entity.get_mut<gameplay::CoreAttributes>(),
        best->entity.get_mut<gameplay::AttributeSet>(),
        best->entity.get_mut<gameplay::AbilitySystem>(),
        best->entity.get_mut<gameplay::CombatState>()
    };
    const auto& playerSys = ctx.playerEntity.get<gameplay::AbilitySystem>();
    gameplay::DamageEvent event =
        gameplay::weaponDamageEvent(weapon, playerSys);
    // C1: a target in its critical window eats the critical execution.
    if (const auto weakness = ctx.gameTags.find("State.CriticalWeakness")) {
        event.critical = block.system.tags.has(*weakness);
    }
    // A5: a guarding NPC catches front-cone hits — damage shrinks, the
    // blocked amount runs the guard's POSTURE down instead. A guard
    // raised inside the perfect window parries CLEAN and the PLAYER'S
    // poise pays for the read attack.
    gameplay::BlockResult guard;
    const Vec3 targetChest = target.entity.get<world::Transform>().position +
                             Vec3 { 0.0f, 1.2f, 0.0f };
    if (const auto blockTag = ctx.gameTags.find("State.Blocking");
        blockTag && block.system.tags.has(*blockTag)) {
        const auto& targetT = target.entity.get<world::Transform>();
        const Vec3 facing = targetT.rotation * Vec3 { 0.0f, 0.0f, 1.0f };
        const auto& defSys = target.entity.get<gameplay::AbilitySystem>();
        guard = gameplay::applyBlock(
            event, facing, targetT.position, body_->position(),
            ctx.statsTuning.blockAngleDegrees, ctx.statsTuning.blockFactor,
            ctx.statsTuning.blockPostureFactor,
            target.entity.get<gameplay::MeleeSwing>().guardSeconds,
            ctx.statsTuning.perfectParryWindow,
            gameplay::currentValueOf(defSys, gameplay::attr("energy")),
            // STATS.md §4: the empty-guard punish.
            gameplay::currentValueOf(defSys,
                                     gameplay::attr("maxPosture")) *
                gameplay::currentValueOf(
                    defSys, gameplay::attr("criticalSensitivity")) /
                100.0f);
        if (guard.perfect) {
            gameplay::StatBlock attacker {
                ctx.playerEntity.get_mut<gameplay::CoreAttributes>(),
                ctx.playerEntity.get_mut<gameplay::AttributeSet>(),
                ctx.playerEntity.get_mut<gameplay::AbilitySystem>(),
                ctx.playerEntity.get_mut<gameplay::CombatState>()
            };
            gameplay::DamageEvent parry;
            parry.postureAmount = ctx.statsTuning.perfectParryPosture;
            gameplay::applyDamage(attacker, parry, ctx.gameTags,
                                  ctx.derivedStats, nullptr,
                                  ctx.statsTuning);
            LOG_INFO("PERFECT PARRY — your poise takes {}",
                     ctx.statsTuning.perfectParryPosture);
            if (ctx.eventBus) {
                ctx.eventBus->dispatch(
                    { gameplay::eventKind("OnParried"),
                      target.entity, ctx.playerEntity });
            }
        } else if (guard.caught) {
            LOG_INFO("Blocked!");
        }
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
    // Combat lifecycle events (BOSS-SCRIPTING §1) — quests, cues and
    // future brains listen on the bus.
    if (ctx.eventBus) {
        gameplay::Event hit;
        hit.kind = gameplay::eventKind("OnHitTaken");
        hit.source = ctx.playerEntity;
        hit.target = target.entity;
        hit.value = result.healthDamage;
        ctx.eventBus->dispatch(hit);
        if (result.staggered) {
            ctx.eventBus->dispatch({ gameplay::eventKind("OnStagger"),
                                     ctx.playerEntity, target.entity });
        }
    }
    // P0 C2: the LOOK of the exchange — one cue per outcome, resolved
    // through CueForms (data): a parry beats a block beats a plain hit.
    if (ctx.cues) {
        if (guard.perfect) {
            ctx.cues->emit({ "Cue.Parry", targetChest,
                             ctx.statsTuning.perfectParryPosture });
        } else if (guard.caught) {
            ctx.cues->emit({ "Cue.Block", targetChest,
                             result.postureDamage });
        } else {
            const gameplay::DamageType type =
                event.channels.empty() ? gameplay::DamageType::Slash
                                       : event.channels[0].type;
            ctx.cues->emit({ str { "Cue.Hit." } +
                                 gameplay::damageTypeName(type),
                             targetChest, result.healthDamage });
        }
    }

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

// P0 D2b — the swim branch: decideMoveMode owns WHEN (sim-pure,
// doctested), this owns HOW. Full-3D wish toward the look, clamped so
// the head never breaches the surface from below; the SwimCost effect
// drains energy on the sprint-cost accumulator pattern (§2.9); an
// exhausted swimmer SINKS and drowns on a periodic unmitigated tick.
bool PlayerController::updateSwimming(f32 dt, const PlayerContext& ctx,
                                      const Vec3& wish, bool moving,
                                      f32 jog, f32 accelRate) {
    const gameplay::StatsTuningForm& tuning = ctx.statsTuning;
    const std::optional<f32> surface =
        ctx.waterSurfaceAt ? ctx.waterSurfaceAt(body_->position())
                           : std::nullopt;
    const gameplay::MoveMode next = gameplay::decideMoveMode(
        moveMode_, surface, body_->position().y, tuning.eyeHeight,
        body_->onGround());
    if (next != moveMode_) {
        // THE transition (dev rule): the facade follows the mode.
        moveMode_ = next;
        body_->setSwimming(moveMode_ == gameplay::MoveMode::Swim);
        swimCostAccumulator = 0.0f;
        drownAccumulator = 0.0f;
    }
    if (moveMode_ != gameplay::MoveMode::Swim) {
        return false;
    }

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
    if (ctx.input.isDown(platform::Key::Space)) {
        wish3.y += 1.0f;
    }
    (void)wish;
    (void)moving;
    const f32 swimSpeed = jog * tuning.swimSpeedFactor;
    Vec3 target = glm::dot(wish3, wish3) > 1e-6f
                      ? glm::normalize(wish3) * swimSpeed
                      : Vec3 { 0.0f };
    if (exhausted) {
        // No strength left: the water wins (STATS.md survival loop).
        target.y = glm::min(target.y, 0.0f) - 1.2f;
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
        swimCostAccumulator += dt;
        while (swimCostAccumulator >= 0.5f) {
            swimCostAccumulator -= 0.5f;
            auto& set = ctx.playerEntity.get_mut<gameplay::AttributeSet>();
            auto& sys = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
            gameplay::applyEffect(set, sys, *ctx.swimCostEffect,
                                  ctx.gameTags);
        }
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
    platform::Input& input = ctx.input;
    // P0 A5: RMB held = raised guard — the State.Blocking tag is the §6
    // vocabulary the damage paths read (both camps). Guarding excludes
    // swinging (and vice versa: the guard waits for the swing to land).
    // R: draw/sheathe (dev design 2026-07-11) — never mid-swing.
    if (input.wasPressed(platform::Key::R) &&
        (!ctx.playerEntity.is_alive() ||
         ctx.playerEntity.get<gameplay::MeleeSwing>().phase ==
             gameplay::SwingPhase::Idle)) {
        weaponDrawn_ = !weaponDrawn_;
    }
    // Ctrl: sneak toggle (dev design 2026-07-12) — the body CROUCHES to
    // half height (standing back up can be refused by a low ceiling),
    // steps soften, detection halves (State.Sneaking drives it all).
    if (input.wasPressed(platform::Key::Ctrl)) {
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
        auto& system = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
        if (const auto tag = ctx.gameTags.find("State.Sneaking")) {
            const bool tagged = system.tags.has(*tag);
            if (sneaking_ && !tagged) {
                system.tags.add(*tag, ctx.gameTags);
            } else if (!sneaking_ && tagged) {
                system.tags.remove(*tag, ctx.gameTags);
            }
        }
    }
    // STATS.md §4: staggered = can't act, parry or dodge, very slow.
    bool staggered = false;
    if (ctx.playerEntity.is_alive()) {
        if (const auto tag = ctx.gameTags.find("State.Staggered")) {
            staggered = ctx.playerEntity.get<gameplay::AbilitySystem>()
                            .tags.has(*tag);
        }
    }
    bool blocking = false;
    if (ctx.playerEntity.is_alive()) {
        auto& system = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
        auto& swing = ctx.playerEntity.get_mut<gameplay::MeleeSwing>();
        blocking = weaponDrawn_ && !staggered && // no guard while reeling
                   input.mouseDown(platform::MouseButton::Right) &&
                   swing.phase == gameplay::SwingPhase::Idle;
        // The guard clock: a hit landing inside the fresh window is a
        // PERFECT parry (applyBlock reads guardSeconds).
        gameplay::tickGuard(swing, blocking, dt);
        if (const auto tag = ctx.gameTags.find("State.Blocking")) {
            const bool tagged = system.tags.has(*tag);
            if (blocking && !tagged) {
                system.tags.add(*tag, ctx.gameTags);
            } else if (!blocking && tagged) {
                system.tags.remove(*tag, ctx.gameTags);
            }
        }
    }
    // B6: melee swing on LMB (the mouse is captured in Play — ImGui
    // never owns it here). Cadence is the ability's cooldown effect plus
    // the swing itself: no hardcoded timer (P0 A3).
    const data::WeaponForm* held = equippedWeapon(ctx);
    if (held && held->projectileSpeed > 0.0f) {
        // A7+: ranged = the CHARGED shot (hold to draw, release to
        // loose); melee inputs stay out of the way.
        updateBowDraw(dt, ctx, *held, blocking || staggered);
    } else {
        if (bowCharge_ >= 0.0f) {
            bowCharge_ = -1.0f; // weapon swapped mid-draw: let it down
        }
        if (!blocking && !staggered &&
            input.mousePressed(platform::MouseButton::Left)) {
            tryAttack(ctx);
        }
    }
    updateSwing(dt, ctx);

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
    // P0 D2b: swimming consumes the whole ground-movement section (no
    // jump/dodge/sprint/strides in the water); the camera/transform sync
    // at the tail still runs.
    if (updateSwimming(dt, ctx, wish, moving, jog, accelRate)) {
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
    // Dodge (dev design 2026-07-11, the 2D arena move in 3D): a TAP on
    // the sprint key — released within dodgeTapSeconds — bursts in the
    // held move direction, backward when none. Cost, cooldown and the
    // State.Dodging i-frames are the Dodge ability's effects (§6).
    if (input.isDown(platform::Key::Shift)) {
        shiftHeldSeconds += dt;
    } else {
        if (shiftHeldSeconds > 0.0f &&
            shiftHeldSeconds <= tuning.dodgeTapSeconds &&
            dodgeTimer <= 0.0f && !staggered && // §4: no dodge while reeling
            ctx.playerEntity.is_alive() &&
            ctx.playerEntity.get<gameplay::MeleeSwing>().phase ==
                gameplay::SwingPhase::Idle) {
            bool activated = true;
            if (ctx.dodgeAbility) {
                auto& set =
                    ctx.playerEntity.get_mut<gameplay::AttributeSet>();
                auto& system =
                    ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
                activated = gameplay::tryActivate(
                    *ctx.dodgeAbility, set, system, set, system,
                    { ctx.forms, ctx.gameTags });
            }
            if (activated) {
                dodgeDir = moving ? glm::normalize(wish) : -forward;
                dodgeTimer = tuning.dodgeDurationSeconds;
            }
        }
        shiftHeldSeconds = 0.0f;
    }

    // C3: overencumbered = no sprint, no jump (STATS.md §3 Utility).
    // A5: no sprint behind a raised guard either.
    const bool sprinting = moving && input.isDown(platform::Key::Shift) &&
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
    // Sneak drain: MOVING sneaked pays (SneakCost, ~3/s, data); holding
    // still and watching is free (dev design 2026-07-12).
    if (sneaking_ && moving && ctx.sneakCostEffect &&
        ctx.playerEntity.is_alive()) {
        sneakCostAccumulator += dt;
        while (sneakCostAccumulator >= 0.5f) {
            sneakCostAccumulator -= 0.5f;
            auto& set = ctx.playerEntity.get_mut<gameplay::AttributeSet>();
            auto& sys = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
            gameplay::applyEffect(set, sys, *ctx.sneakCostEffect,
                                  ctx.gameTags);
        }
    } else if (!moving) {
        sneakCostAccumulator = 0.0f;
    }

    // Eyes above the feet (eyeHeight, §5 U4-7; a crouch halves it —
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
        // A5: the entity FACES where the camera looks (rotation * +Z =
        // horizontal camera forward, the NPC yaw convention) — the guard
        // cone and any future sim consumer read this, never the camera.
        transform.rotation = glm::angleAxis(glm::pi<f32>() - yaw,
                                            Vec3 { 0.0f, 1.0f, 0.0f });
    }
}

} // namespace game
