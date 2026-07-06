#include "game/SaveGame.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "data/forms/FormDatabase.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/TomlWriter.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Paths.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "world/scene/Components.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace game {

std::optional<data::Record> captureReference(ecs::Entity entity,
                                             const data::FormDatabase& forms) {
    if (!entity.is_alive() || !entity.has<world::RefId>()) {
        return std::nullopt;
    }
    const auto& refId = entity.get<world::RefId>();
    const auto* reference =
        forms.find<world::ReferenceForm>(refId.referenceId);
    if (!reference) {
        return std::nullopt; // prefab child without a record — B7's brick
    }
    const reflect::TypeInfo& type = world::ReferenceForm::staticTypeInfo();

    data::Record record;
    record.formId = refId.referenceId;
    record.typeId = type.id;
    record.creates = false;

    // Cell: the entity's current home vs the resolved record.
    core::Guid liveCell {};
    if (refId.cell.isValid()) {
        if (const data::Form* cellForm = forms.get(refId.cell)) {
            liveCell = cellForm->id;
        }
    }
    if (liveCell != reference->cell) {
        record.fields.emplace(type.findField("cell")->id,
                              reflect::Value { liveCell });
    }

    // Transform: actors only (see header note).
    if (entity.has<world::ActorMarker>() &&
        entity.has<world::Transform>()) {
        const auto& transform = entity.get<world::Transform>();
        if (transform.position != reference->position) {
            record.fields.emplace(type.findField("position")->id,
                                  reflect::Value { transform.position });
        }
        if (transform.rotation != reference->rotation) {
            record.fields.emplace(type.findField("rotation")->id,
                                  reflect::Value { transform.rotation });
        }
    }

    if (record.fields.empty()) {
        return std::nullopt;
    }
    return record;
}

// --- PendingSaveLayer ---------------------------------------------------------------

PendingSaveLayer::Entry& PendingSaveLayer::entryFor(
    const core::Guid& referenceId) {
    return entries[referenceId];
}

void PendingSaveLayer::captureEntity(ecs::Entity entity,
                                     const data::FormDatabase& forms,
                                     const gameplay::GameplayTagRegistry& tags) {
    if (!entity.is_alive() || !entity.has<world::RefId>()) {
        return;
    }
    const core::Guid refGuid = entity.get<world::RefId>().referenceId;
    if (!refGuid.isValid()) {
        return; // prefab-derived child without identity — B7
    }
    Entry& entry = entryFor(refGuid);
    entry.referencePatch = captureReference(entity, forms);
    entry.materialized = false;
    entry.actorRecords.clear();
    if (entity.has<gameplay::AbilitySystem>()) {
        entry.actorRecords = gameplay::captureActor(entity, refGuid, tags);
    }
    if (!entry.referencePatch && entry.actorRecords.empty() &&
        !entry.disabled) {
        entries.erase(refGuid); // nothing worth remembering
    }
}

void PendingSaveLayer::captureCell(ecs::World& world,
                                   const data::FormDatabase& forms,
                                   ecs::Entity cellEntity,
                                   const gameplay::GameplayTagRegistry& tags) {
    // Unload-time event — building the query here is fine (never per
    // frame). Collect first: captureEntity mutates no structure, but keep
    // the iteration read-only anyway.
    vector<ecs::Entity> members;
    world.handle()
        .query_builder<const world::RefId>()
        .with<ecs::InCell>(cellEntity)
        .build()
        .each([&](flecs::entity e, const world::RefId&) {
            members.push_back(ecs::Entity { e });
        });
    for (ecs::Entity entity : members) {
        captureEntity(entity, forms, tags);
    }
}

void PendingSaveLayer::disableReference(const core::Guid& referenceId) {
    Entry& entry = entryFor(referenceId);
    entry.disabled = true;
    data::Record patch;
    patch.formId = referenceId;
    patch.typeId = world::ReferenceForm::staticTypeInfo().id;
    patch.creates = false;
    patch.fields.emplace(
        world::ReferenceForm::staticTypeInfo().findField("enabled")->id,
        reflect::Value { false });
    entry.referencePatch = std::move(patch);
}

bool PendingSaveLayer::isEnabled(const core::Guid& referenceId) const {
    const auto it = entries.find(referenceId);
    return it == entries.end() || !it->second.disabled;
}

bool PendingSaveLayer::hasActorState(const core::Guid& referenceId) const {
    const auto it = entries.find(referenceId);
    return it != entries.end() && !it->second.actorRecords.empty();
}

gameplay::SavedActorRecords PendingSaveLayer::actorState(
    const core::Guid& referenceId) {
    gameplay::SavedActorRecords saved;
    const auto it = entries.find(referenceId);
    if (it == entries.end()) {
        return saved;
    }
    Entry& entry = it->second;
    if (!entry.materialized) {
        entry.effects.clear();
        entry.items.clear();
        entry.injuries.clear();
        bool hasStats = false;
        for (const data::Record& record : entry.actorRecords) {
            if (record.typeId ==
                gameplay::SavedStatsForm::staticTypeInfo().id) {
                entry.stats =
                    gameplay::formFromRecord<gameplay::SavedStatsForm>(
                        record);
                hasStats = true;
            } else if (record.typeId ==
                       gameplay::SavedEffectForm::staticTypeInfo().id) {
                entry.effects.push_back(
                    gameplay::formFromRecord<gameplay::SavedEffectForm>(
                        record));
            } else if (record.typeId ==
                       gameplay::SavedItemForm::staticTypeInfo().id) {
                entry.items.push_back(
                    gameplay::formFromRecord<gameplay::SavedItemForm>(
                        record));
            } else if (record.typeId ==
                       gameplay::SavedInjuryForm::staticTypeInfo().id) {
                entry.injuries.push_back(
                    gameplay::formFromRecord<gameplay::SavedInjuryForm>(
                        record));
            }
        }
        entry.materialized = hasStats;
    }
    if (entry.materialized) {
        saved.stats = &entry.stats;
        for (const auto& effect : entry.effects) {
            saved.effects.push_back(&effect);
        }
        for (const auto& item : entry.items) {
            saved.items.push_back(&item);
        }
        for (const auto& injury : entry.injuries) {
            saved.injuries.push_back(&injury);
        }
    }
    return saved;
}

vector<data::Record> PendingSaveLayer::flush() const {
    // Deterministic order (§8): by reference guid, patches first.
    vector<const Entry*> ordered;
    vector<core::Guid> keys;
    keys.reserve(entries.size());
    for (const auto& [key, entry] : entries) {
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    vector<data::Record> records;
    for (const core::Guid& key : keys) {
        const Entry& entry = entries.at(key);
        if (entry.referencePatch) {
            records.push_back(*entry.referencePatch);
        }
        records.insert(records.end(), entry.actorRecords.begin(),
                       entry.actorRecords.end());
    }
    return records;
}

void PendingSaveLayer::clear() {
    entries.clear();
}

// --- Save files ---------------------------------------------------------------------

std::filesystem::path savesDirectory() {
    return platform::executableDir() / "saves";
}

std::filesystem::path savePath(const str& slot) {
    return savesDirectory() / (slot + ".toml");
}

vector<str> listSaveSlots() {
    struct Slot {
        str name;
        std::filesystem::file_time_type time;
    };
    vector<Slot> slots;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator { savesDirectory(), ec }) {
        if (!entry.is_regular_file() ||
            entry.path().extension() != ".toml") {
            continue;
        }
        slots.push_back({ entry.path().stem().string(),
                          entry.last_write_time(ec) });
    }
    std::sort(slots.begin(), slots.end(),
              [](const Slot& a, const Slot& b) { return a.time > b.time; });
    vector<str> names;
    names.reserve(slots.size());
    for (Slot& slot : slots) {
        names.push_back(std::move(slot.name));
    }
    return names;
}

bool writeSave(const str& slot, const data::Plugin& plugin,
               const data::FormTypeRegistry& types) {
    std::error_code ec;
    std::filesystem::create_directories(savesDirectory(), ec);
    std::ofstream out { savePath(slot), std::ios::binary };
    if (!out) {
        LOG_ERROR("save: cannot write {}", savePath(slot).string());
        return false;
    }
    out << data::writePluginToml(plugin, types);
    LOG_INFO("Saved: {} ({} records)", savePath(slot).string(),
             plugin.records.size());
    return true;
}

std::optional<data::Plugin> readSave(const str& slot,
                                     const data::FormTypeRegistry& types) {
    std::ifstream in { savePath(slot), std::ios::binary };
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return data::parsePluginToml(text.str(), types, slot);
}

} // namespace game
