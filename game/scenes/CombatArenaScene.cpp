#include "game/scenes/CombatArenaScene.hpp"

#include <cmath>
#include <utility>

#include <glm/glm.hpp>
#include <imgui.h>

#include "engine/core/Guid.hpp"
#include "engine/platform/Input.hpp"
#include "engine/platform/Window.hpp" // width()/height() for aspect + projection
#include "engine/render/Camera2D.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/actors/CharacterTick.hpp"
#include "gameplay/combat/Combat.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp" // CombatState
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

// Player controller tuning (placeholders; balance pass in Step 5).
// movementSpeed/acceleration are stat-space (docs/STATS.md §3); these map them to
// world units (units/s and units/s²). Calibrated so the default sheet (~102)
// walks at ~5 u/s and ramps up in ~0.18s.
constexpr f32 kSpeedScale = 0.05f; // movementSpeed (stat) → world units/s
constexpr f32 kAccelScale = 0.28f; // acceleration  (stat) → world units/s²
constexpr f32 kBrakeMult = 1.6f;   // braking inertia is sharper than accel
constexpr f32 kDodgeSpeed = 14.0f;
constexpr f32 kDodgeTime = 0.28f; // must match DodgeIFrames.durationSeconds

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

void CombatArenaScene::onEnter() {
    WorldDemoScene::onEnter();

    // Combat/status tag vocabulary needed by tickCharacter (life state, stagger,
    // paralysis, the nine status types) plus the stats runtime tags (injuries,
    // survival). Same set StatsScene registers.
    tags.registerTag("State.Dead");
    tags.registerTag("State.Staggered");
    tags.registerTag("State.Paralyzed");
    for (const char* statusTag :
         { "Status.Poisoned", "Status.Bleeding", "Status.Mental", "Status.Diseased",
           "Status.Cursed", "Status.Dying", "Status.HarmonyBroken", "Status.Ignited",
           "Status.Glaciated", "Status.Electrocuted" }) {
        tags.registerTag(statusTag);
    }
    gameplay::registerStatsRuntimeTags(tags); // Injury.Active, Internal.Survival*

    // Dodge ability tags: the i-frame state, its cooldown, and the shared
    // energy-exhaustion gate (blockedTag on every energy-costed ability).
    tags.registerTag("State.Dodging");
    tags.registerTag("Cooldown.Dodge");
    tags.registerTag("State.Exhausted");
    dodgeAbility = forms.find<gameplay::AbilityForm>(kDodgeAbility);

    // The player (blue sheet), facing south — toward the camera.
    player = spawnCombatant("Player", { 0.0f, -4.0f, 0.0f }, kPlayerSheet,
                            { 0.0f, -1.0f });

    // Two training dummies (enemy sheet), facing the player. Real enemy AI (and
    // dynamic facing toward their target) lands in Step 4; for now they just tick.
    spawnCombatant("Dummy A", { -2.0f, 2.0f, 0.0f }, kEnemySheet, { 0.0f, -1.0f });
    spawnCombatant("Dummy B", { 2.0f, 2.0f, 0.0f }, kEnemySheet, { 0.0f, -1.0f });
}

void CombatArenaScene::update(f32 dt) {
    // Player input → velocity + facing + dodge, before the stats tick so the
    // dodge ability's cost/tags are visible to this frame's tickCharacter.
    updatePlayer(dt);

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
        gameplay::tickCharacter(c.entity, dt, gameDt, ctx);
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

    // WASD → normalized walk direction (independent of facing, FPS-style).
    Vec3 walk { 0.0f, 0.0f, 0.0f };
    if (input.isDown(platform::Key::W)) walk.y += 1.0f;
    if (input.isDown(platform::Key::S)) walk.y -= 1.0f;
    if (input.isDown(platform::Key::D)) walk.x += 1.0f;
    if (input.isDown(platform::Key::A)) walk.x -= 1.0f;
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

void CombatArenaScene::drawUi() {
    ImGui::Begin("Combat Arena (Step 2: player control)");
    ImGui::TextWrapped(
        "WASD to move, aim with the mouse, Shift to dodge (costs energy, has a "
        "cooldown, blocked while Exhausted). Attacks and enemy AI land next.");
    ImGui::Separator();

    const auto deadTag = tags.find("State.Dead");
    const auto staggeredTag = tags.find("State.Staggered");
    const auto paralyzedTag = tags.find("State.Paralyzed");

    if (ImGui::BeginTable("combatants", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Health");
        ImGui::TableSetupColumn("Energy");
        ImGui::TableSetupColumn("Essence");
        ImGui::TableSetupColumn("Posture");
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
            ImGui::TableNextColumn();
            const bool dead = deadTag && system.tags.has(*deadTag);
            const bool staggered = staggeredTag && system.tags.has(*staggeredTag);
            const bool paralyzed = paralyzedTag && system.tags.has(*paralyzedTag);
            ImGui::TextUnformatted(dead        ? "DEAD"
                                   : paralyzed ? "Paralyzed"
                                   : staggered ? "Staggered"
                                               : "OK");
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace game
