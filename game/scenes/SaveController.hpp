#pragma once

#include <functional>
#include <optional>

#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp" // ecs::Entity
#include "data/forms/Form.hpp"  // data::FormHandle (by value)
#include "game/SaveGame.hpp"    // PendingSaveLayer, data::Plugin

namespace data {
class FormDatabase;
class FormTypeRegistry;
}
namespace gameplay {
class GameplayTagRegistry;
struct GameClock;
}
namespace quest {
struct QuestLog;
}

namespace game {

// The scene state performSave reads, bundled so the disk-save serialization
// is decoupled from LandscapeScene (audit U4-1). Rebuilt per save (cheap) —
// references plus the world snapshot the WorldStateForm records (clock,
// worldspace, camera, mode, weather) and the two scene actions the save
// needs as closures (sweeping the live references, the toast).
struct SaveContext {
    data::FormDatabase& forms;
    const data::FormTypeRegistry& formTypes;
    const gameplay::GameplayTagRegistry& gameTags;
    const quest::QuestLog& questLog;
    const gameplay::GameClock& gameClock;
    data::FormHandle activeWorldspace; // resolved to its id in the record
    f32 playerYaw { 0.0f };
    f32 playerPitch { 0.0f };
    bool playMode { true };
    i32 weatherSelected { -1 };
    // Sweep every live reference entity (the loaded cells' contents + the
    // persistent player) so performSave can capture each into the layer.
    std::function<void(const std::function<void(ecs::Entity)>&)> forEachLiveRef;
    std::function<void(const str&)> notify; // interaction.say(msg, 3s)
};

// The disk-save orchestration extracted from LandscapeScene (audit U4-1):
// the capture-everything-live + flush-the-pending-layer serialization
// (performSave), the queued-reload flags (requestLoad / takeReloadRequest),
// and the load-file resolution the scene re-enters with (beginLoad). It OWNS
// the pending in-memory layer — the memory of unloaded cells shared with
// streaming (capture/veto) and actor spawn (override/state apply) — exposed
// through pending() so those call sites stay put (the hud.inventory()
// pattern). The load-APPLICATION half (WorldStateForm → clock/worldspace/
// camera restore) stays woven into the scene's onEnter lifecycle.
class SaveController {
public:
    // Capture everything live + flush the pending layer into one ordinary
    // plugin (§5) written to saves/<slot>.toml.
    void performSave(const SaveContext& ctx, const str& slot);

    // Queue a reload: notifies (and does nothing else) when the slot has no
    // file; otherwise the next update() re-enters the scene with the save
    // resolved as the LAST layer.
    void requestLoad(const str& slot,
                     const std::function<void(const str&)>& notify);
    // update() end: true (once) when a reload is queued — the scene then
    // exits+re-enters.
    bool takeReloadRequest();

    // bootstrapData: resolve the queued slot's file as the last layer.
    // Returns the save plugin (nullopt when none queued / not found) and
    // records whether this session came from a save (loadedFromSave()).
    std::optional<data::Plugin> beginLoad(const data::FormTypeRegistry& types);
    bool loadedFromSave() const { return loadedFromSave_; }

    // The pending in-memory layer — streaming hooks (captureCell/isEnabled),
    // pickups (disableReference) and actor spawn (applyReferenceOverrides/
    // actorState) reach it here. Fresh per scene enter.
    PendingSaveLayer& pending() { return pendingSave_; }

private:
    PendingSaveLayer pendingSave_;
    str pendingLoadSlot_;            // consumed by the next onEnter
    bool reloadRequested_ { false }; // exit+enter at the end of update()
    bool loadedFromSave_ { false };  // this session came from a save file
};

} // namespace game
