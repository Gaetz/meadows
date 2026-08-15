#include "game/scenes/CombatArenaScene.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <glm/glm.hpp>
#include <imgui.h>

#include "data/forms/CoreForms.hpp" // WeaponForm
#include "engine/core/Guid.hpp"
#include "engine/platform/Input.hpp"
#include "engine/platform/Window.hpp" // width()/height() for aspect + projection
#include "engine/render/Camera2D.hpp"
#include "engine/render/SpriteRenderer.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/actors/CharacterTick.hpp"
#include "gameplay/combat/Combat.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp" // CombatState, applyDamage
#include "gameplay/inventory/Inventory.hpp" // Equipment
#include "gameplay/stats/EquipmentStats.hpp" // weaponDamageEvent, applyEquipmentModifiers
#include "gameplay/stats/Injuries.hpp"
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/ResonanceDecays.hpp"
#include "gameplay/stats/StatsTuning.hpp" // registerStatsRuntimeTags
#include "gameplay/stats/StatusBuildup.hpp"
#include "gameplay/stats/Survival.hpp"
#include "world/scene/Collision.hpp"
#include "world/scene/Components.hpp"
#include "world/scene/Movement.hpp"

namespace game {

namespace {

// 8-direction placeholder sheets (declared in base.toml). One 512x64 strip = 8
// frames of 64x64; the frame is selected via uvRect from a facing direction.
const core::Guid kPlayerSheet =
    *core::Guid::fromString("b1a7c0de-0000-4000-8000-000000000001");
const core::Guid kEnemySheet =
    *core::Guid::fromString("b1a7c0de-0000-4000-8000-000000000002");

// Dodge ability (declared in base.toml): self cost/cooldown/i-frames, gated by
// State.Exhausted. Activated via tryActivate on Shift press.
const core::Guid kDodgeAbility =
    *core::Guid::fromString("ab000000-0000-4000-8000-000000000002");

// Player attack ability + default weapon (declared in base.toml). The dodge
// ability guid (kDodgeAbility) is defined with the sheet guids above.
const core::Guid kPlayerAttack =
    *core::Guid::fromString("ab000000-0000-4000-8000-000000000003");
const core::Guid kIronSword =
    *core::Guid::fromString("3d8b1f6a-92c4-4e07-b8d9-1a5c7e30f482");
const core::Guid kMace =
    *core::Guid::fromString("3d8b1f6a-92c4-4e07-b8d9-1a5c7e30f483");
const core::Guid kPoisonDagger =
    *core::Guid::fromString("3d8b1f6a-92c4-4e07-b8d9-1a5c7e30f484");
const core::Guid kBleedMace =
    *core::Guid::fromString("3d8b1f6a-92c4-4e07-b8d9-1a5c7e30f485");
const core::Guid kFlameScimitar =
    *core::Guid::fromString("3d8b1f6a-92c4-4e07-b8d9-1a5c7e30f486");
const core::Guid kLeatherArmor =
    *core::Guid::fromString("a5000000-0000-4000-8000-000000000001");
const core::Guid kIronArmor =
    *core::Guid::fromString("a5000000-0000-4000-8000-000000000002");

// [cpp-tuning] Player controller tuning (2D GAS test bench — the DEMO's
// equivalents live in StatsTuningForm, §5; promote these too if this
// scene ever needs modding). movementSpeed/acceleration are stat-space
// (docs/STATS.md §3); these map them to world units (units/s and
// units/s²). Calibrated so the default sheet (~102) walks at ~5 u/s.
constexpr f32 kSpeedScale = 0.05f; // movementSpeed (stat) → world units/s
constexpr f32 kAccelScale = 0.28f; // acceleration  (stat) → world units/s²
constexpr f32 kBrakeMult = 1.6f;   // braking inertia is sharper than accel
constexpr f32 kDodgeSpeed = 14.0f;
constexpr f32 kDodgeTime = 0.28f; // must match DodgeIFrames.durationSeconds

// [cpp-tuning] Melee attack tuning (2D bench). A short swing with a
// front arc; the active window is when the hitbox is live.
constexpr f32 kAttackWindup = 0.12f;   // telegraph before the hit lands
constexpr f32 kAttackActive = 0.10f;   // hitbox live
constexpr f32 kAttackRecovery = 0.18f; // follow-through, still committed
constexpr f32 kAttackRange = 1.4f;     // reach from player center (world units)
constexpr f32 kAttackArcCos = 0.5f;    // half-arc = 60° (dot ≥ cos60°)

// Selects one frame's uvRect in an 8-frame horizontal strip from a 2D facing
// (world +Y up). Frame i = angle i*45 deg CCW from +X (East) — matches
// tools/gen_placeholder_sprites.py. A zero direction maps to frame 0 (East).
//
// The sprite quad maps world +Y to v=1, and textures are uploaded un-flipped
// (row 0 = top), so the renderer samples with V pointing DOWN in world space —
// a vertical mirror. We negate facing.y here to cancel it, so the on-screen
// facing matches the world-space aim direction. (Horizontal is unaffected.)
Vec4 uvRectForFacing8(Vec2 facing) {
    constexpr i32 kFrames = 8;
    constexpr f32 kQuarterTurn = 0.785398163f; // pi/4
    f32 angle = 0.0f;
    if (facing.x != 0.0f || facing.y != 0.0f) {
        angle = std::atan2(-facing.y, facing.x);
    }
    i32 frame = static_cast<i32>(std::lround(angle / kQuarterTurn));
    frame = ((frame % kFrames) + kFrames) % kFrames;
    const f32 u0 = static_cast<f32>(frame) / static_cast<f32>(kFrames);
    const f32 u1 = static_cast<f32>(frame + 1) / static_cast<f32>(kFrames);
    return { u0, 0.0f, u1, 1.0f };
}

} // namespace

ecs::Entity CombatArenaScene::spawnCombatant(std::string name, Vec3 position,
                                             const core::Guid& sheet, Vec2 facing) {
    ecs::Entity e = world.create();

    world::Transform transform;
    transform.position = position;
    e.set<world::Transform>(transform);

    world::SpriteRender sprite;
    sprite.sprite = sheet;
    sprite.uvRect = uvRectForFacing8(facing); // pick the facing frame
    sprite.layer = 1;
    e.set<world::SpriteRender>(sprite);

    // Movement/collision bodies are attached now (harmless while static) so the
    // Step 2 player controller and Step 4 enemy AI have them ready.
    e.set<world::Velocity>({});
    e.set<world::Collider>({ Vec2 { 0.4f, 0.4f }, false });

    // Full character-stats sheet (§2.7): the same set the Spawner attaches to
    // data-driven actors, so tickCharacter runs uniformly on every combatant.
    e.set<gameplay::CoreAttributes>({});
    e.set<gameplay::AttributeSet>({});
    e.set<gameplay::AbilitySystem>({});
    e.set<gameplay::Resonance>({});
    e.set<gameplay::Survival>({});
    e.set<gameplay::StatusBuildup>({});
    e.set<gameplay::CombatState>({});
    e.set<gameplay::Injuries>({});
    e.set<gameplay::ResonanceDecays>({});
    e.add<world::ActorMarker>();

    gameplay::initializeCurrent(e.get_mut<gameplay::AbilitySystem>(),
                                e.get<gameplay::AttributeSet>());

    // Recompute derived stats and fill vitals/posture to full.
    const gameplay::CharacterTickContext ctx { derived, tags, tuning };
    gameplay::initializeActorStats(e, ctx);

    combatants.push_back({ e, std::move(name) });
    return e;
}

gameplay::StatModifiers CombatArenaScene::equipmentModsFor(ecs::Entity e) const {
    gameplay::StatModifiers mods;
    if (const auto* eq = e.try_get<gameplay::Equipment>()) {
        gameplay::applyEquipmentModifiers(*eq, forms, mods);
    }
    return mods;
}

void CombatArenaScene::onEnter() {
    WorldDemoScene::onEnter();

    // Proof handler: any hit cue bursts sparks at the impact point.
    // (The real runtime resolves CueForms — CueTable — into particle/
    // sound/shake handlers; one hardwired handler proves the seam.)
    cues.addHandler([this](const gameplay::CueEvent& event) {
        fx::EmitterParams sparks;
        sparks.burst = 14;
        sparks.lifetime = 0.28f;
        sparks.lifetimeJitter = 0.10f;
        sparks.velocity = { 0.0f, 2.5f, 0.0f };
        sparks.velocityJitter = 3.5f;
        sparks.gravity = { 0.0f, -9.0f, 0.0f };
        sparks.sizeStart = 0.16f;
        sparks.sizeEnd = 0.02f;
        sparks.colorStart = { 1.0f, 0.9f, 0.35f, 1.0f };
        sparks.colorEnd = { 1.0f, 0.25f, 0.05f, 0.0f };
        // Cosmetic seed: position hash + running count (free RNG, §8).
        const u32 seed = static_cast<u32>(static_cast<i32>(event.position.x * 73.0f)) ^
                         (static_cast<u32>(static_cast<i32>(event.position.y * 179.0f)) << 8) ^
                         particles.count();
        particles.spawn(sparks, event.position, seed);
    });

    // The shared character-tick vocabulary (life state, statuses, buildup,
    // stats runtime tags) — one aggregator for every scene.
    gameplay::registerCharacterRuntimeTags(tags);

    // Dodge ability tags: the i-frame state and the cooldowns (the
    // exhaustion gate is part of the shared vocabulary).
    tags.registerTag("State.Dodging");
    tags.registerTag("Cooldown.Dodge");
    tags.registerTag("Cooldown.Attack");
    dodgeAbility = forms.find<gameplay::AbilityForm>(kDodgeAbility);
    attackAbility = forms.find<gameplay::AbilityForm>(kPlayerAttack);
    weapons[0] = forms.find<data::WeaponForm>(kIronSword);     // 1: classic sword
    weapons[1] = forms.find<data::WeaponForm>(kMace);          // 2: mace
    weapons[2] = forms.find<data::WeaponForm>(kPoisonDagger);  // 3: poison dagger
    weapons[3] = forms.find<data::WeaponForm>(kBleedMace);     // 4: bleed mace
    weapons[4] = forms.find<data::WeaponForm>(kFlameScimitar); // 5: flame scimitar

    // The player (blue sheet), facing south — toward the camera.
    player = spawnCombatant("Player", { 0.0f, -4.0f, 0.0f }, kPlayerSheet,
                            { 0.0f, -1.0f });

    // Three training dummies (enemy sheet): unarmored / leather / iron, so the
    // typed-damage mitigation is directly comparable. Real enemy AI (and dynamic
    // facing) lands in Step 4; for now they just tick. Equip via a torso ArmorForm
    // and re-initialize with the folded mods so they start armored.
    const gameplay::CharacterTickContext ctx { derived, tags, tuning };
    const auto equipDummy = [&](ecs::Entity e, const core::Guid& armor) {
        gameplay::Equipment eq;
        eq.torso = armor;
        e.set<gameplay::Equipment>(eq);
        gameplay::initializeActorStats(e, ctx, equipmentModsFor(e));
    };
    spawnCombatant("Dummy (none)", { -3.0f, 2.0f, 0.0f }, kEnemySheet, { 0.0f, -1.0f });
    equipDummy(spawnCombatant("Dummy (leather)", { 0.0f, 2.0f, 0.0f }, kEnemySheet,
                              { 0.0f, -1.0f }),
               kLeatherArmor);
    equipDummy(spawnCombatant("Dummy (iron)", { 3.0f, 2.0f, 0.0f }, kEnemySheet,
                              { 0.0f, -1.0f }),
               kIronArmor);
}

void CombatArenaScene::update(f32 dt) {
    // Player input → velocity + facing + dodge, then the melee swing, before the
    // stats tick so ability costs/tags and dealt damage are visible to this
    // frame's tickCharacter (which refreshes life state).
    updatePlayer(dt);
    updatePlayerAttack(dt);
    particles.update(dt); // cue sparks

    // Advance the shared game clock once, then run the full per-frame tick on
    // every combatant. We deliberately do NOT call WorldDemoScene::update(): its
    // loop calls tickEffects on all actors, which tickCharacter already does —
    // calling both would double-tick durations and cooldowns.
    const f64 gameDt = clock.advance(dt);
    const gameplay::CharacterTickContext ctx { derived, tags, tuning };
    for (const Combatant& c : combatants) {
        if (!c.entity.is_alive()) {
            continue;
        }
        gameplay::tickCharacter(c.entity, dt, gameDt, ctx, equipmentModsFor(c.entity));
        // Idempotent life-state refresh (cheap): guards any path that drops
        // health to 0 without going through applyDamage.
        gameplay::updateLifeState(c.entity.get_mut<gameplay::AbilitySystem>(), tags);
    }

    // Integrate movement and push combatants out of solids. Dummies have zero
    // velocity (idle) until enemy AI lands in Step 4.
    world::applyMovement(world, dt);
    world::resolveCollisions(world);
}

void CombatArenaScene::updatePlayer(f32 dt) {
    if (!player.is_alive()) {
        return;
    }

    const platform::Input& input = engine.getInput();

    // Weapon switch (keys 1-5), ignoring slots that failed to resolve.
    const platform::Key weaponKeys[kWeaponCount] = {
        platform::Key::Num1, platform::Key::Num2, platform::Key::Num3,
        platform::Key::Num4, platform::Key::Num5,
    };
    for (int i = 0; i < kWeaponCount; ++i) {
        if (input.wasPressed(weaponKeys[i]) && weapons[i] != nullptr) {
            weaponIndex = i;
        }
    }

    // WASD → normalized walk direction (independent of facing, FPS-style).
    const Vec2 axis = platform::moveAxis(input);
    Vec3 walk { axis.x, axis.y, 0.0f };
    const bool moving = (walk.x != 0.0f || walk.y != 0.0f);
    if (moving) {
        walk = glm::normalize(walk);
    }

    // Facing = vector from the player to the mouse cursor in world space.
    const auto& transform = player.get<world::Transform>();
    const Vec2 playerPos { transform.position.x, transform.position.y };
    const f32 aspect = static_cast<f32>(engine.getWindow().width()) /
                       static_cast<f32>(engine.getWindow().height());
    const Vec2 worldMouse = render::screenToWorld(
        engine.getCamera(), input.mousePosition(), aspect,
        engine.getWindow().width(), engine.getWindow().height());
    const Vec2 toMouse = worldMouse - playerPos;
    if (glm::dot(toMouse, toMouse) > 1e-6f) {
        aimDir = glm::normalize(toMouse);
    }
    player.get_mut<world::SpriteRender>().uvRect = uvRectForFacing8(aimDir);

    // Dodge: a real GameplayAbility. tryActivate rejects it if on cooldown, if
    // State.Exhausted is set, or if the energy cost is unaffordable — so no
    // manual gating here. On success it pays the cost, starts the cooldown, and
    // grants State.Dodging; the scene only drives the movement burst.
    if (input.wasPressed(platform::Key::Shift) && dodgeAbility != nullptr) {
        auto& set = player.get_mut<gameplay::AttributeSet>();
        auto& sys = player.get_mut<gameplay::AbilitySystem>();
        const gameplay::AbilityContext ctx { forms, tags };
        if (gameplay::tryActivate(*dodgeAbility, set, sys, set, sys, ctx)) {
            dodgeDir = moving ? Vec2 { walk.x, walk.y } : -aimDir;
            dodgeTimer = kDodgeTime;
        }
    }

    // Velocity. Dodge burst overrides the walk while active; otherwise the walk
    // velocity ramps toward its target instead of snapping (docs/STATS.md §3
    // acceleration + braking inertia), so movement has weight.
    const auto& sys = player.get<gameplay::AbilitySystem>();
    const f32 targetSpeed =
        gameplay::currentValueOf(sys, gameplay::attr("movementSpeed")) * kSpeedScale;
    const f32 accel =
        gameplay::currentValueOf(sys, gameplay::attr("acceleration")) * kAccelScale;

    const Vec3 velNow = player.get<world::Velocity>().value;
    Vec2 vel { velNow.x, velNow.y };

    if (dodgeTimer > 0.0f) {
        // Dodge burst eases out — full speed at the start, decelerating to a stop
        // by the end (ease-out quad on the remaining fraction), not a hard cut.
        dodgeTimer = std::max(0.0f, dodgeTimer - dt);
        const f32 r = dodgeTimer / kDodgeTime; // remaining fraction, 1 → 0
        const f32 ease = r * (2.0f - r);       // ease-out: holds, then slows
        vel = dodgeDir * (kDodgeSpeed * ease);
    } else {
        // Move toward the desired velocity at `accel`; brake harder when idle.
        const Vec2 desired = moving ? Vec2 { walk.x, walk.y } * targetSpeed
                                    : Vec2 { 0.0f, 0.0f };
        const f32 rate = moving ? accel : accel * kBrakeMult;
        const Vec2 delta = desired - vel;
        const f32 dist = glm::length(delta);
        const f32 step = rate * dt;
        vel = (dist <= step || dist < 1e-5f) ? desired : vel + (delta / dist) * step;
    }
    player.set<world::Velocity>({ Vec3 { vel.x, vel.y, 0.0f } });
}

void CombatArenaScene::updatePlayerAttack(f32 dt) {
    if (!player.is_alive()) {
        return;
    }

    // Start a swing on left-click when idle (and not mid-dodge): the attack
    // ability gates the energy cost, cooldown, and State.Exhausted. On success,
    // lock the aim for the whole swing and enter windup.
    const platform::Input& input = engine.getInput();
    if (attackPhase == AttackPhase::None && dodgeTimer <= 0.0f &&
        input.mousePressed(platform::MouseButton::Left) && attackAbility != nullptr) {
        auto& set = player.get_mut<gameplay::AttributeSet>();
        auto& sys = player.get_mut<gameplay::AbilitySystem>();
        const gameplay::AbilityContext ctx { forms, tags };
        if (gameplay::tryActivate(*attackAbility, set, sys, set, sys, ctx)) {
            attackPhase = AttackPhase::Windup;
            attackTimer = kAttackWindup;
            attackDir = aimDir;
            hitThisSwing.clear();
        }
    }

    if (attackPhase == AttackPhase::None) {
        return;
    }

    // Advance the windup → active → recovery timeline.
    attackTimer -= dt;
    if (attackTimer <= 0.0f) {
        switch (attackPhase) {
        case AttackPhase::Windup:
            attackPhase = AttackPhase::Active;
            attackTimer = kAttackActive;
            break;
        case AttackPhase::Active:
            attackPhase = AttackPhase::Recovery;
            attackTimer = kAttackRecovery;
            break;
        case AttackPhase::Recovery:
        case AttackPhase::None:
            attackPhase = AttackPhase::None;
            attackTimer = 0.0f;
            break;
        }
    }

    // Only the active window deals damage. Sweep the front arc and hit each enemy
    // at most once per swing with the weapon's typed damage (applyDamage does the
    // mitigation + posture/stagger pipeline).
    const data::WeaponForm* weapon = weapons[weaponIndex];
    if (attackPhase != AttackPhase::Active || weapon == nullptr) {
        return;
    }
    const auto& playerT = player.get<world::Transform>();
    const Vec2 origin { playerT.position.x, playerT.position.y };
    auto& playerSys = player.get_mut<gameplay::AbilitySystem>();

    for (const Combatant& c : combatants) {
        if (c.entity == player || !c.entity.is_alive()) {
            continue;
        }
        if (std::find(hitThisSwing.begin(), hitThisSwing.end(), c.entity) !=
            hitThisSwing.end()) {
            continue;
        }
        const auto& targetT = c.entity.get<world::Transform>();
        const Vec2 to { targetT.position.x - origin.x, targetT.position.y - origin.y };
        const f32 dist = glm::length(to);
        if (dist > kAttackRange || dist < 1e-4f) {
            continue;
        }
        if (glm::dot(to / dist, attackDir) < kAttackArcCos) {
            continue; // outside the swing arc
        }

        auto& core = c.entity.get_mut<gameplay::CoreAttributes>();
        auto& vitals = c.entity.get_mut<gameplay::AttributeSet>();
        auto& system = c.entity.get_mut<gameplay::AbilitySystem>();
        auto& combat = c.entity.get_mut<gameplay::CombatState>();
        gameplay::StatBlock block { core, vitals, system, combat };
        const gameplay::StatModifiers mods = equipmentModsFor(c.entity);
        gameplay::applyDamage(block, gameplay::weaponDamageEvent(*weapon, playerSys),
                              tags, derived, &mods, tuning);
        // Weapon status buildup on hit (poison/bleed/ignition…), gated by the
        // target's endurance (armor raises it), so the mitigation is live.
        if (!weapon->buildupType.empty() && weapon->buildupAmount > 0.0f) {
            auto& buildup = c.entity.get_mut<gameplay::StatusBuildup>();
            gameplay::tryAddBuildup(buildup,
                                    gameplay::parseStatusType(weapon->buildupType),
                                    weapon->buildupAmount, system, tags);
        }
        gameplay::updateLifeState(system, tags);
        // The sim announces the impact as a CUE — presentation-agnostic
        // (headless = no handler = no-op). This scene's handler sparks.
        cues.emit({ "Cue.Hit.Slash", targetT.position,
                    currentValueOf(system, gameplay::attr("damage")) });
        hitThisSwing.push_back(c.entity);
    }
}

void CombatArenaScene::draw(render::SpriteRenderer& renderer) {
    WorldDemoScene::draw(renderer);
    // Cue sparks over the world (painter order: after every entity).
    particles.forEach([&](const Vec3& position, f32 size,
                          const Vec4& color, bool) {
        renderer.draw({ .position = { position.x, position.y },
                        .size = { size, size },
                        .tint = color,
                        .texture = checker });
    });
}

void CombatArenaScene::drawUi() {
    ImGui::Begin("Combat Arena (Step 3: melee attack)");
    ImGui::TextWrapped(
        "WASD move, mouse aim, Shift dodge, LEFT-CLICK swing. Keys 1-5 swap weapon "
        "to test each damage type + effect against the unarmored/leather/iron dummies.");

    // Current weapon + its full profile (damage / posture / effect).
    ImGui::SeparatorText("Weapon [1-5]");
    if (const data::WeaponForm* w = weapons[weaponIndex]) {
        ImGui::Text("%d. %s", weaponIndex + 1, w->displayName.c_str());
        const auto chan = [](const char* n, f32 v) {
            if (v > 0.0f) {
                ImGui::SameLine();
                ImGui::Text("| %s %.0f", n, v);
            }
        };
        chan("slash", w->slashAttack);
        chan("blunt", w->bluntAttack);
        chan("pierce", w->pierceAttack);
        chan("fire", w->fireAttack);
        chan("lightning", w->lightningAttack);
        ImGui::SameLine();
        ImGui::Text("| posture %.0f", w->postureDamage);
        if (!w->buildupType.empty()) {
            ImGui::SameLine();
            ImGui::Text("| %s +%.0f", w->buildupType.c_str(), w->buildupAmount);
        }
    }
    ImGui::Separator();

    const auto deadTag = tags.find("State.Dead");
    const auto staggeredTag = tags.find("State.Staggered");
    const auto paralyzedTag = tags.find("State.Paralyzed");

    const auto poisonedTag = tags.find("Status.Poisoned");
    const auto bleedingTag = tags.find("Status.Bleeding");
    const auto ignitedTag = tags.find("Status.Ignited");
    if (ImGui::BeginTable("combatants", 9,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Health");
        ImGui::TableSetupColumn("Energy");
        ImGui::TableSetupColumn("Essence");
        ImGui::TableSetupColumn("Posture");
        ImGui::TableSetupColumn("Poison");   // buildup / endurance threshold
        ImGui::TableSetupColumn("Bleed");
        ImGui::TableSetupColumn("Ignition");
        ImGui::TableSetupColumn("State");
        ImGui::TableHeadersRow();

        for (const Combatant& c : combatants) {
            if (!c.entity.is_alive()) {
                continue;
            }
            const auto& system = c.entity.get<gameplay::AbilitySystem>();
            const auto& combat = c.entity.get<gameplay::CombatState>();
            const auto cur = [&](const char* n) {
                return gameplay::currentValueOf(system, gameplay::attr(n));
            };

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(c.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.0f / %.0f", cur("health"), cur("maxHealth"));
            ImGui::TableNextColumn();
            ImGui::Text("%.0f / %.0f", cur("energy"), cur("maxEnergy"));
            ImGui::TableNextColumn();
            ImGui::Text("%.0f / %.0f", cur("essence"), cur("maxEssence"));
            ImGui::TableNextColumn();
            ImGui::Text("%.0f / %.0f", combat.posture, cur("maxPosture"));
            const auto& buildup = c.entity.get<gameplay::StatusBuildup>();
            ImGui::TableNextColumn();
            ImGui::Text("%.0f / %.0f", buildup.poison, cur("endurancePoison"));
            ImGui::TableNextColumn();
            ImGui::Text("%.0f / %.0f", buildup.bleed, cur("enduranceBleed"));
            ImGui::TableNextColumn();
            ImGui::Text("%.0f / %.0f", buildup.ignition, cur("enduranceIgnition"));
            ImGui::TableNextColumn();
            const auto has = [&](const std::optional<gameplay::GameplayTag>& t) {
                return t && system.tags.has(*t);
            };
            ImGui::TextUnformatted(has(deadTag)        ? "DEAD"
                                   : has(paralyzedTag) ? "Paralyzed"
                                   : has(staggeredTag) ? "Staggered"
                                   : has(bleedingTag)  ? "Bleeding"
                                   : has(ignitedTag)   ? "Ignited"
                                   : has(poisonedTag)  ? "Poisoned"
                                                       : "OK");
        }
        ImGui::EndTable();
    }

    // Debug: the weapons (keys 1-5) cover slash/blunt/pierce/fire + poison/bleed/
    // ignition. Lightning has no weapon, so keep one button to show iron's negative
    // lightning resistance (it takes MORE). Reset heals every dummy to full.
    ImGui::SeparatorText("Debug");
    if (ImGui::Button("Lightning 40 (no weapon covers it)")) {
        for (const Combatant& c : combatants) {
            if (c.entity == player || !c.entity.is_alive()) {
                continue;
            }
            auto& core = c.entity.get_mut<gameplay::CoreAttributes>();
            auto& vitals = c.entity.get_mut<gameplay::AttributeSet>();
            auto& system = c.entity.get_mut<gameplay::AbilitySystem>();
            auto& combat = c.entity.get_mut<gameplay::CombatState>();
            gameplay::StatBlock block { core, vitals, system, combat };
            const gameplay::StatModifiers mods = equipmentModsFor(c.entity);
            gameplay::applyDamage(
                block, gameplay::DamageEvent { { { gameplay::DamageType::Lightning, 40.0f } }, 0.0f },
                tags, derived, &mods, tuning);
            gameplay::updateLifeState(system, tags);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset dummies (heal to full)")) {
        const gameplay::CharacterTickContext ctx { derived, tags, tuning };
        for (const Combatant& c : combatants) {
            if (c.entity == player || !c.entity.is_alive()) {
                continue;
            }
            gameplay::initializeActorStats(c.entity, ctx, equipmentModsFor(c.entity));
        }
    }

    ImGui::End();
}

} // namespace game
