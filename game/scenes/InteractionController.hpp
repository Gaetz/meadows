#pragma once

#include <functional>

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"
#include "engine/ecs/World.hpp"       // ecs::Entity, flecs::query
#include "world/scene/Components.hpp" // world::Transform, DoorTarget, RefId

namespace platform {
class Input;
}
namespace data {
class FormDatabase;
class TextTable;
}
namespace phys {
class PhysicsWorld;
class CharacterBody;
}
namespace gameplay {
struct GameClock;
struct StatsTuningForm;
}

namespace game {

struct Npc;
class PendingSaveLayer;

// The scene systems the generic [E] interaction touches, bundled so the
// prompt scan, the travel/rest fade machine, and the talk toast are decoupled
// from LandscapeScene (audit U4-10). The scene rebuilds it each call from its
// own members — references, a few scalars, and the actions that stay scene
// territory as closures (the SculptContext publish pattern). Mirrors
// EditorContext / StreamingContext / NpcContext.
struct InteractionContext {
    data::FormDatabase& forms;
    flecs::query<const world::Transform, const world::DoorTarget>& doorQuery;
    flecs::query<const world::Transform, const world::RefId>& interactQuery;
    const vector<uptr<Npc>>& npcs; // dead actors are searched, not talked to
    platform::Input& input;
    gameplay::GameClock& gameClock;
    const gameplay::StatsTuningForm& statsTuning;
    const data::TextTable& texts; // U4-11: player-facing strings by key
    PendingSaveLayer& pendingSave; // item pickup flushes enabled = false
    phys::PhysicsWorld* physics;   // fade-in floor probe
    phys::CharacterBody* player;   // the aiming eye
    ecs::Entity playerEntity;
    Vec3 cameraForward;
    bool playMode; // mode == Play (prompts only exist in first person)
    // Scene actions fired by [E] or at the black of the fade:
    std::function<void(const core::Guid& targetReference)> travel;
    std::function<void(ecs::Entity partner, const core::Guid& dialogue)>
        openDialogue;
    std::function<void(ecs::Entity container)> openContainer;
    std::function<bool(const str& screen)> tryShowScreen; // workstation UI
};

// Generic interaction extracted from LandscapeScene (audit U4-10): the aim
// scan + [E] prompt (doors, items, actors, corpses, furniture), the fade
// state machine that carries travel and rest through black, and the talk
// toast. performTravel itself STAYS in the scene (a worldspace swap is
// streaming/scene territory); the machine fires it through ctx.travel.
class InteractionController {
public:
    // Per frame while the sim runs: prompt scan, [E] dispatch, fade advance.
    void update(f32 dt, const InteractionContext& ctx);

    // Menus' "wait N hours": clock + needs decay, NO bed recovery (B6).
    void wait(f32 hours, const InteractionContext& ctx);

    // Arm a travel through the fade (doors do this internally; onEnter's
    // dev-start-interior boot uses it directly).
    void beginTravel(const core::Guid& targetReference);

    // The toast line over the HUD (quest updates, saves, crime...).
    void say(str line, f32 seconds);

    // onEnter re-init: clear fade, pending actions, prompt and toast.
    void reset();

    // HUD/ImGui reads (the scene ANDs its own playMode where relevant).
    bool promptVisible() const {
        return promptEntity.is_alive() && fadeDirection == 0 &&
               !promptLabel_.empty();
    }
    const str& promptLabel() const { return promptLabel_; }
    bool talkVisible() const {
        return talkTimer > 0.0f && !talkLine_.empty();
    }
    const str& talkLine() const { return talkLine_; }
    f32 fadeAlpha() const { return fadeAlpha_; }
    bool fading() const { return fadeDirection != 0; }

private:
    // Chantier 3 B7-lite: rest/sleep on furniture, at the black of the
    // fade — the Phase-7 sleep() advances the game clock (the sky follows
    // on the next frame), decays hunger/thirst over the skipped time,
    // restores the sleep need, and accrues Rest (the injury/resonance
    // recovery precondition). NPC schedules re-evaluate on their next
    // slot check and warp forward.
    void rest(f32 hours, const InteractionContext& ctx);

    // Chantier 3 B1: GENERIC interaction (E) — doors travel, items land
    // in the inventory, actors talk, furniture rests (B7).
    enum class PromptKind : u8 { None, Door, Item, Actor, Corpse,
                                 Furniture };
    ecs::Entity promptEntity {};
    PromptKind promptKind { PromptKind::None };
    str promptLabel_;
    str talkLine_; // placeholder dialogue bubble / toast
    f32 talkTimer { 0.0f };
    core::Guid pendingTravel {};    // armed target marker reference
    f32 pendingSleepHours { 0.0f }; // armed rest/sleep (B7-lite), at black
    f32 fadeAlpha_ { 0.0f };        // 0 = clear, 1 = black
    i32 fadeDirection { 0 };        // +1 fading out, -1 fading in
    // Travel fade: extra seconds spent holding at black while the arrival
    // floor's collider is still cooking (see the fade-in probe).
    f32 fadeHoldSeconds { 0.0f };
};

} // namespace game
