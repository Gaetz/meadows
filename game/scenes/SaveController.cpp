#include "game/scenes/SaveController.hpp"

#include <filesystem>

#include "data/forms/Form.hpp"
#include "data/forms/FormDatabase.hpp"
#include "data/forms/LocForms.hpp" // TextTable (C9.5)
#include "data/plugins/Record.hpp"
#include "engine/core/Guid.hpp"
#include "engine/core/Log.hpp"
#include "gameplay/save/SaveForms.hpp"  // WorldStateForm
#include "gameplay/save/SaveState.hpp"  // createRecord
#include "gameplay/stats/GameClock.hpp"
#include "quest/Quest.hpp"              // captureQuestLog, QuestLog

namespace game {

void SaveController::performSave(const SaveContext& ctx, const str& slot) {
    // Capture EVERYTHING live (loaded cells' entities + the persistent
    // player) into the pending layer, then flush it plus the world state
    // into one ordinary plugin (§5). Sweep order is the flush's sorted
    // order — deterministic (§8).
    ctx.forEachLiveRef([&](ecs::Entity entity) {
        pendingSave_.captureEntity(entity, ctx.forms, ctx.gameTags);
    });

    data::Plugin plugin;
    plugin.id = *core::Guid::fromString(
        "5a5e0000-0000-4000-8000-000000000001"); // the one save layer
    plugin.name = "save-" + slot;
    plugin.records = pendingSave_.flush();
    // Chantier 6 A4: the quest log (scene-level, rebuilt fresh each save
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

    if (writeSave(slot, plugin, ctx.formTypes)) {
        ctx.notify(ctx.texts.format("save.saved", slot)); // C9.5
    }
}

void SaveController::requestLoad(
    const str& slot, const data::TextTable& texts,
    const std::function<void(const str&)>& notify) {
    if (!std::filesystem::exists(savePath(slot))) {
        notify(texts.format("save.missing", slot)); // C9.5
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
    // Chantier 5 B5: a loading game resolves its save file as the LAST
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
