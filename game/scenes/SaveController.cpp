#include "game/scenes/SaveController.hpp"

#include <chrono>
#include <filesystem>

#include "data/forms/Form.hpp"
#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp" // copied into the save worker
#include "data/forms/LocForms.hpp" // TextTable
#include "data/plugins/Record.hpp"
#include "engine/core/Guid.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "gameplay/save/SaveForms.hpp"  // WorldStateForm
#include "gameplay/save/SaveState.hpp"  // createRecord
#include "gameplay/stats/GameClock.hpp"
#include "quest/Quest.hpp"              // captureQuestLog, QuestLog

namespace game {

namespace {

f64 msSince(const std::chrono::steady_clock::time_point& start) {
    return std::chrono::duration<f64, std::milli> {
        std::chrono::steady_clock::now() - start
    }.count();
}

} // namespace

void SaveController::performSave(const SaveContext& ctx, const str& slot) {
    if (!flightGate_.requestStart(slot)) {
        return; // one save in flight — the slot is remembered (last wins)
                // and relaunched by pumpCompletions with a fresh capture
    }
    // Capture EVERYTHING live (loaded cells' entities + the persistent
    // player) into the pending layer, then flush it plus the world state
    // into one ordinary plugin (§5). This half stays ON the frame — the
    // world must not move under the capture, and the sweep order is the
    // flush's sorted order — deterministic (§8).
    const auto captureStart = std::chrono::steady_clock::now();
    ctx.forEachLiveRef([&](ecs::Entity entity) {
        pendingSave_.captureEntity(entity, ctx.forms, ctx.gameTags);
    });

    data::Plugin plugin;
    plugin.id = *core::Guid::fromString(
        "5a5e0000-0000-4000-8000-000000000001"); // the one save layer
    plugin.name = "save-" + slot;
    plugin.records = pendingSave_.flush();
    // The quest log (scene-level, rebuilt fresh each save
    // like the WorldStateForm — never in the pending layer).
    const auto questRecords = quest::captureQuestLog(ctx.questLog);
    plugin.records.insert(plugin.records.end(), questRecords.begin(),
                          questRecords.end());

    gameplay::WorldStateForm state;
    state.gameSeconds = ctx.gameClock.gameSeconds;
    state.timescale = ctx.gameClock.timescale;
    if (ctx.activeWorldspace.isValid()) {
        if (const data::Form* space = ctx.forms.get(ctx.activeWorldspace)) {
            state.activeWorldspace = space->id;
        }
    }
    state.playerYaw = ctx.playerYaw;
    state.playerPitch = ctx.playerPitch;
    state.playMode = ctx.playMode;
    state.weatherSelected = ctx.weatherSelected;
    plugin.records.push_back(gameplay::createRecord(
        state, *core::Guid::fromString(
                   "5a5e0000-0000-4000-8000-0000000000ff")));
    const f64 captureMs = msSince(captureStart);

    // The serialization + file IO leave the frame. The worker owns
    // an INDEPENDENT plugin (moved — pendingSave_ keeps mutating after
    // this frame; flush() already built a fresh vector) and a COPY of the
    // type registry (pointer maps over static TypeInfos — cheap), and it
    // captures the shared completion queue, NEVER `this` (the
    // ResidencyCache teardown idiom).
    auto work = [sharedRef = shared_, slot, plugin = std::move(plugin),
                 types = ctx.formTypes, captureMs] {
        SaveCompletion done;
        done.slot = slot;
        done.recordCount = static_cast<u32>(plugin.records.size());
        done.captureMs = captureMs;
        const auto serializeStart = std::chrono::steady_clock::now();
        const str text = serializeSave(plugin, types);
        done.serializeMs = msSince(serializeStart);
        const auto writeStart = std::chrono::steady_clock::now();
        done.ok = writeSaveText(slot, text); // tmp + rename — atomic
        done.writeMs = msSince(writeStart);
        sharedRef->completions.push(std::move(done));
    };
    if (ctx.jobs) {
        ctx.jobs->enqueue(std::move(work));
    } else {
        work(); // headless / no JobSystem: same path, on the caller
    }
}

std::optional<str> SaveController::pumpCompletions(
    const data::TextTable& texts,
    const std::function<void(const str&)>& notify) {
    std::optional<str> relaunch;
    shared_->completions.drain([&](SaveCompletion&& done) {
        if (done.ok) {
            LOG_INFO("Saved '{}': {} records (capture {:.1f} ms, "
                     "serialize {:.1f} ms, write {:.1f} ms)",
                     done.slot, done.recordCount, done.captureMs,
                     done.serializeMs, done.writeMs);
            if (notify) {
                notify(texts.format("save.saved", done.slot));
            }
        } // (failure already LOG_ERRORed by writeSaveText, no toast)
        // The gate reopens; a save requested while this one flew is
        // relaunched by the SCENE with a fresh SaveContext (fresh capture).
        if (auto deferred = flightGate_.onComplete()) {
            relaunch = std::move(*deferred);
        }
    });
    return relaunch;
}

void SaveController::requestLoad(
    const str& slot, const data::TextTable& texts,
    const std::function<void(const str&)>& notify) {
    if (!std::filesystem::exists(savePath(slot))) {
        notify(texts.format("save.missing", slot));
        return;
    }
    pendingLoadSlot_ = slot;
    reloadRequested_ = true; // consumed at the end of update()
}

bool SaveController::takeReloadRequest() {
    if (!reloadRequested_) {
        return false;
    }
    reloadRequested_ = false;
    return true;
}

std::optional<data::Plugin> SaveController::beginLoad(
    const data::FormTypeRegistry& types) {
    // A loading game resolves its save file as the LAST
    // layer — one more plugin, the §5 invariant in action.
    loadedFromSave_ = false;
    if (pendingLoadSlot_.empty()) {
        return std::nullopt;
    }
    std::optional<data::Plugin> savePlugin = readSave(pendingLoadSlot_, types);
    if (savePlugin) {
        LOG_INFO("Loading save '{}' ({} records)", pendingLoadSlot_,
                 savePlugin->records.size());
        loadedFromSave_ = true;
    } else {
        LOG_WARN("save '{}' not found", pendingLoadSlot_);
    }
    pendingLoadSlot_.clear();
    return savePlugin;
}

} // namespace game
