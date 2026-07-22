#pragma once

#include <functional>
#include <optional>

#include "engine/core/ConcurrentQueue.hpp" // async save completions
#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp" // ecs::Entity
#include "data/forms/Form.hpp"  // data::FormHandle (by value)
#include "game/SaveGame.hpp"    // PendingSaveLayer, SaveFlightGate, data::Plugin

namespace core {
class JobSystem;
}
namespace data {
class FormDatabase;
class FormTypeRegistry;
class TextTable;
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
// is decoupled from LandscapeScene. Rebuilt per save (cheap) —
// references plus the world snapshot the WorldStateForm records (clock,
// worldspace, camera, mode, weather) and the two scene actions the save
// needs as closures (sweeping the live references, the toast).
struct SaveContext {
    data::FormDatabase& forms;
    const data::FormTypeRegistry& formTypes;
    const data::TextTable& texts; // The save.saved toast
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
    // Where the serialize + file IO run. Null = synchronous on the
    // calling thread (headless tests, one-shot tools) — same code path,
    // the completion still lands in the pump.
    core::JobSystem* jobs { nullptr };
};

// The disk-save orchestration extracted from LandscapeScene:
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
    // plugin (§5) written to saves/<slot>.toml. The capture + plugin
    // build stay ON the frame (sim coherence §8, same order as always);
    // the TOML serialization + file IO run on ctx.jobs — the completion
    // (timing log + toast) lands in pumpCompletions. Single-flight: a
    // request while one is in flight remembers the LAST slot, relaunched
    // on completion with a fresh capture.
    void performSave(const SaveContext& ctx, const str& slot);

    // Main thread, once per frame at a fixed point (next to the residency
    // pumps): drains finished saves — the timing log and the save.saved
    // toast fire HERE, at completion. Returns the deferred slot to
    // re-save (nullopt when none): the scene relaunches performSave with
    // a FRESH context so the deferred save captures the current world.
    std::optional<str> pumpCompletions(
        const data::TextTable& texts,
        const std::function<void(const str&)>& notify);

    // Queue a reload: notifies (and does nothing else) when the slot has no
    // file; otherwise the next update() re-enters the scene with the save
    // resolved as the LAST layer. `texts` localizes the missing-slot toast.
    void requestLoad(const str& slot, const data::TextTable& texts,
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
    // What the worker reports back through the completion queue.
    struct SaveCompletion {
        str slot;
        bool ok { false };
        u32 recordCount { 0 };
        f64 captureMs { 0.0 };   // frame-side: capture + flush + build
        f64 serializeMs { 0.0 }; // worker-side: TOML text
        f64 writeMs { 0.0 };     // worker-side: tmp write + rename
    };
    // Outlives the controller: workers capture the shared_ptr, never
    // `this` (the ResidencyCache teardown idiom — a save finishing after
    // scene teardown pushes harmlessly into a queue only it still owns).
    struct Shared {
        core::ConcurrentQueue<SaveCompletion> completions;
    };

    PendingSaveLayer pendingSave_;
    sptr<Shared> shared_ { std::make_shared<Shared>() };
    SaveFlightGate flightGate_;      // One save in flight, last wins
    str pendingLoadSlot_;            // consumed by the next onEnter
    bool reloadRequested_ { false }; // exit+enter at the end of update()
    bool loadedFromSave_ { false };  // this session came from a save file
};

} // namespace game
