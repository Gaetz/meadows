#pragma once

#include <string>
#include <vector>

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"
#include "engine/ecs/World.hpp"
#include "game/scenes/WorldDemoScene.hpp"

namespace gameplay {
struct AbilityForm;
}

namespace game {

// Phase 8 — the playable "village + arena" scene (docs/PHASE-8.md).
//
// Step 1 (foundation): a player + training dummies, all driven by the full
// `tickCharacter` pipeline every frame — the move from the ImGui-driven
// single-actor StatsScene to a live multi-combatant simulation. Player control,
// attacks, enemy AI, and the rest/merchant NPCs land in the following steps.
class CombatArenaScene : public WorldDemoScene {
public:
    using WorldDemoScene::WorldDemoScene;

    void onEnter() override;
    void update(f32 dt) override;
    void drawUi() override;

private:
    // A spawned combatant plus a display name for the debug readout.
    struct Combatant {
        ecs::Entity entity;
        std::string name;
    };

    // Creates a combatant entity with the full character-stats sheet (same
    // component set the Spawner attaches to data-driven actors) at full vitals.
    // `sheet` is an 8-direction sprite-sheet asset; `facing` picks the frame.
    ecs::Entity spawnCombatant(std::string name, Vec3 position,
                               const core::Guid& sheet, Vec2 facing);

    // Step 2 player controller: WASD movement, facing toward the mouse cursor,
    // and the dodge ability. Runs before the per-combatant tick each frame.
    void updatePlayer(f32 dt);

    ecs::Entity player {};
    std::vector<Combatant> combatants; // player + dummies (ticked + inspected)

    // Dodge state (Step 2). The GAS ability owns cost/cooldown/i-frames; the
    // scene only drives the movement burst and the sprite facing.
    const gameplay::AbilityForm* dodgeAbility { nullptr }; // resolved in onEnter
    f32 dodgeTimer { 0.0f };            // > 0 → dodge velocity burst in progress
    Vec2 dodgeDir { 0.0f, 0.0f };       // burst direction
    Vec2 aimDir { 0.0f, -1.0f };        // current facing (default south)
};

} // namespace game
