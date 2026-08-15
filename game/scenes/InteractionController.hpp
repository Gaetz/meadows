#pragma once

#include <functional>

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"
#include "engine/ecs/World.hpp"       // ecs::Entity, flecs::query
#include "game/InputActions.hpp"      // game::ActionMap
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
// from LandscapeScene. The scene rebuilds it each call from its
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
    const data::TextTable& texts; // Player-facing strings by key
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
    // [E] on a DOWNED ally — the scene routes it to
    // FollowerController::reviveDownedAlly (potion from the player's bag).
    std::function<void(ecs::Entity ally)> reviveAlly;
    // [E]/[X] fires through the action layer, never a raw key.
    const ActionMap* actions { nullptr };
    // [E] on a grave = the homage (toast + cue —
    // scene territory); [F] on a dead FOLLOWER's corpse = bury him here
    // (FollowerController::buryOnSpot behind the closure).
    std::function<void(ecs::Entity grave)> homage;
    std::function<void(ecs::Entity corpse)> buryCorpse;
    // v1: [E] on a mount (furniture category
    // "mount", the grave idiom) — the scene destroys the capsule and
    // hands the frame over to RideController.
    std::function<void(ecs::Entity mount)> mountRide;
    // [E] on a container furniture (mine chest): the scene lazily rolls
    // the form's loadout into the Inventory, then opens the transfer UI.
    std::function<void(ecs::Entity chest)> openChest;
    // [E] on a lever (dungeon locks): the scene opens the paired barrier
    // (world::barrierForLever inverts lever reference -> barrier).
    std::function<void(ecs::Entity lever)> pullLever;
    // Sleep in a bed, fired at the black of the fade: the scene runs
    // gameplay::sleepCharacter over the player (clock, needs, regen,
    // injury recovery — the time-skip path).
    std::function<void(f32 hours)> applySleep;
    // Wait (campfire): same skip WITHOUT the sleep-need restore.
    std::function<void(f32 hours)> applyWait;
};

// Generic interaction extracted from LandscapeScene: the aim
// scan + [E] prompt (doors, items, actors, corpses, furniture), the fade
// state machine that carries travel and rest through black, and the talk
// toast. performTravel itself STAYS in the scene (a worldspace swap is
// streaming/scene territory); the machine fires it through ctx.travel.
class InteractionController {
public:
    // Per frame while the sim runs: prompt scan, [E] dispatch, fade advance.
    void update(f32 dt, const InteractionContext& ctx);

    // Menus' "wait N hours": clock + needs decay, NO bed recovery.
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
    // Rest/sleep on furniture, at the black of the
    // fade — gameplay::sleep() advances the game clock (the sky follows
    // on the next frame), decays hunger/thirst over the skipped time,
    // restores the sleep need, and accrues Rest (the injury/resonance
    // recovery precondition). NPC schedules re-evaluate on their next
    // slot check and warp forward.
    void rest(f32 hours, const InteractionContext& ctx);

    // GENERIC interaction (E) — doors travel, items land
    // in the inventory, actors talk, furniture rests.
    enum class PromptKind : u8 { None, Door, Item, Actor, Corpse,
                                 Furniture,
                                 DownedAlly, // heal a downed follower
                                 Grave,      // homage [E] / deposit [F]
                                 Mount,      // ride it
                                 Container,  // loot it (mine chest)
                                 Lever };    // pull it (dungeon locks)
    ecs::Entity promptEntity {};
    PromptKind promptKind { PromptKind::None };
    str promptLabel_;
    str talkLine_; // placeholder dialogue bubble / toast
    f32 talkTimer { 0.0f };
    core::Guid pendingTravel {};    // armed target marker reference
    f32 pendingSleepHours { 0.0f }; // armed rest/sleep, at black
    f32 fadeAlpha_ { 0.0f };        // 0 = clear, 1 = black
    i32 fadeDirection { 0 };        // +1 fading out, -1 fading in
    // Travel fade: extra seconds spent holding at black while the arrival
    // floor's collider is still cooking (see the fade-in probe).
    f32 fadeHoldSeconds { 0.0f };
};

} // namespace game
