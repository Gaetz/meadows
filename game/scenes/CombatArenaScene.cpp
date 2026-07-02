#include "game/scenes/CombatArenaScene.hpp"

#include <cmath>
#include <utility>

#include <imgui.h>

#include "engine/core/Guid.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
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
#include "world/scene/Components.hpp"

namespace game {

namespace {

// 8-direction placeholder sheets (declared in base.toml). One 512x64 strip = 8
// frames of 64x64; the frame is selected via uvRect from a facing direction.
const core::Guid kPlayerSheet =
    *core::Guid::fromString("b1a7c0de-0000-4000-8000-000000000001");
const core::Guid kEnemySheet =
    *core::Guid::fromString("b1a7c0de-0000-4000-8000-000000000002");

// Selects one frame's uvRect in an 8-frame horizontal strip from a 2D facing
// (world +Y up). Frame i = angle i*45 deg CCW from +X (East) — matches
// tools/gen_placeholder_sprites.py. A zero direction maps to frame 0 (East).
Vec4 uvRectForFacing8(Vec2 facing) {
    constexpr i32 kFrames = 8;
    constexpr f32 kQuarterTurn = 0.785398163f; // pi/4
    f32 angle = 0.0f;
    if (facing.x != 0.0f || facing.y != 0.0f) {
        angle = std::atan2(facing.y, facing.x);
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

    // The player (blue sheet), facing south — toward the camera. Player control
    // lands in Step 2; for now it just ticks and shows a fixed frame.
    player = spawnCombatant("Player", { 0.0f, -4.0f, 0.0f }, kPlayerSheet,
                            { 0.0f, -1.0f });

    // Two training dummies (enemy sheet), facing the player. Real enemy AI (and
    // dynamic facing toward their target) lands in Step 4; for now they just tick.
    spawnCombatant("Dummy A", { -2.0f, 2.0f, 0.0f }, kEnemySheet, { 0.0f, -1.0f });
    spawnCombatant("Dummy B", { 2.0f, 2.0f, 0.0f }, kEnemySheet, { 0.0f, -1.0f });
}

void CombatArenaScene::update(f32 dt) {
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
}

void CombatArenaScene::drawUi() {
    ImGui::Begin("Combat Arena (Step 1: live tick)");
    ImGui::TextWrapped(
        "Every combatant is driven by the full tickCharacter pipeline each "
        "frame. Player control, attacks, and enemy AI land in the next steps.");
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
