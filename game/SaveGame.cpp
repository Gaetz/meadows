#include "game/SaveGame.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>

#include "data/forms/FormDatabase.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/TomlWriter.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Paths.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/inventory/Inventory.hpp" // É8: grave capture gate
#include "world/scene/Components.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace game {

namespace {

// Builds a full `creates` ReferenceForm record from a live entity (the
// prefab-derived-child path — those references exist in no plugin).
std::optional<data::Record> materializeReference(
    ecs::Entity entity, const data::FormDatabase& forms, bool enabled) {
    const auto& refId = entity.get<world::RefId>();
    world::ReferenceForm derived;
    derived.id = refId.referenceId;
    if (const data::Form* base = forms.get(refId.base)) {
        derived.baseForm = base->id;
    }
    if (refId.cell.isValid()) {
        if (const data::Form* cell = forms.get(refId.cell)) {
            derived.cell = cell->id;
        }
    }
    if (entity.has<world::Transform>()) {
        const auto& transform = entity.get<world::Transform>();
        derived.position = transform.position;
        derived.rotation = transform.rotation;
        derived.scale = transform.scale;
    }
    derived.enabled = enabled;
    data::Record record = gameplay::createRecord(derived, derived.id);
    if (!enabled) {
        // createRecord drops default-equal fields; enabled=false differs
        // from the default so it is already carried — this is just belt
        // and braces for a default change.
        record.fields.emplace(
            world::ReferenceForm::staticTypeInfo().findField("enabled")->id,
            reflect::Value { false });
    }
    return record;
}

} // namespace

std::optional<data::Record> captureReference(ecs::Entity entity,
                                             const data::FormDatabase& forms) {
    if (!entity.is_alive() || !entity.has<world::RefId>()) {
        return std::nullopt;
    }
    const auto& refId = entity.get<world::RefId>();
    const auto* reference =
        forms.find<world::ReferenceForm>(refId.referenceId);
    if (!reference) {
        // A prefab-derived child: no plugin creates its record, so a
        // patch would be an orphan the resolver drops. Materialize it as
        // a FULL `creates` record under its deterministic derived guid —
        // the Spawner's expansion steps aside when the record exists.
        return materializeReference(entity, forms, /*enabled=*/true);
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

    // Position/rotation: actors only (see header note — capturing a
    // ground-snapped item/static Y would double the offset on reload).
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
    // Scale: every entity — the ground snap never touches it, so the diff
    // is snap-safe, and the materialize path already carried it (a scale
    // change on an existing reference was silently lost — audit U5-5).
    if (entity.has<world::Transform>()) {
        const auto& transform = entity.get<world::Transform>();
        if (transform.scale != reference->scale) {
            record.fields.emplace(type.findField("scale")->id,
                                  reflect::Value { transform.scale });
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
    // FOLLOWERS É8 (appended clause): an Inventory-only entity — a grave —
    // captures too. captureActor skips every missing component and still
    // emits the SavedStatsForm sentinel + the SavedItemForm rows, so the
    // grave's content survives the flush/re-resolve round trip.
    if (entity.has<gameplay::AbilitySystem>() ||
        entity.has<gameplay::Inventory>()) {
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

void PendingSaveLayer::disableReference(const core::Guid& referenceId,
                                        const data::FormDatabase& forms,
                                        ecs::Entity entity) {
    Entry& entry = entryFor(referenceId);
    entry.disabled = true;
    if (!forms.find<world::ReferenceForm>(referenceId) &&
        entity.is_alive() && entity.has<world::RefId>()) {
        // Prefab-derived child: materialize the full disabled record (a
        // patch to a record no plugin creates is dropped as an orphan).
        if (auto record =
                materializeReference(entity, forms, /*enabled=*/false)) {
            entry.referencePatch = std::move(*record);
            return;
        }
    }
    data::Record patch;
    patch.formId = referenceId;
    patch.typeId = world::ReferenceForm::staticTypeInfo().id;
    patch.creates = false;
    patch.fields.emplace(
        world::ReferenceForm::staticTypeInfo().findField("enabled")->id,
        reflect::Value { false });
    entry.referencePatch = std::move(patch);
}

void PendingSaveLayer::createReference(const core::Guid& referenceId,
                                       const core::Guid& baseForm,
                                       const core::Guid& cell,
                                       const Vec3& position,
                                       const Quat& rotation) {
    // FOLLOWERS É8: the materializeReference idiom without a live entity —
    // build the full ReferenceForm and diff it into a `creates` record
    // (createRecord drops default-equal fields; a null cell therefore
    // resolves back to the persistent set). Idempotent per guid: a second
    // call simply rewrites the same record.
    world::ReferenceForm created;
    created.id = referenceId;
    created.baseForm = baseForm;
    created.cell = cell;
    created.position = position;
    created.rotation = rotation;
    Entry& entry = entryFor(referenceId);
    entry.disabled = false;
    entry.referencePatch = gameplay::createRecord(created, created.id);
}

bool PendingSaveLayer::isEnabled(const core::Guid& referenceId) const {
    const auto it = entries.find(referenceId);
    return it == entries.end() || !it->second.disabled;
}

bool PendingSaveLayer::isRehomed(const core::Guid& referenceId) const {
    // FOLLOWERS É1: read the answer off the captured patch itself — a
    // `cell` diff means the reference lives somewhere else now (see the
    // header). No parallel state to keep in sync with captureEntity; a
    // later capture that homes it back (dismiss) lifts the veto.
    const auto it = entries.find(referenceId);
    if (it == entries.end() || !it->second.referencePatch) {
        return false;
    }
    const u32 cellFieldId =
        world::ReferenceForm::staticTypeInfo().findField("cell")->id;
    return it->second.referencePatch->fields.contains(cellFieldId);
}

void PendingSaveLayer::applyReferenceOverrides(
    ecs::Entity entity, const core::Guid& referenceId) const {
    const auto it = entries.find(referenceId);
    if (it == entries.end() || !it->second.referencePatch ||
        !entity.is_alive() || !entity.has<world::Transform>()) {
        return;
    }
    const data::Record& patch = *it->second.referencePatch;
    const reflect::TypeInfo& type = world::ReferenceForm::staticTypeInfo();
    auto& transform = entity.get_mut<world::Transform>();
    if (const reflect::FieldInfo* field = type.findField("position")) {
        if (const auto f = patch.fields.find(field->id);
            f != patch.fields.end()) {
            if (const Vec3* p = std::get_if<Vec3>(&f->second)) {
                transform.position = *p;
            }
        }
    }
    if (const reflect::FieldInfo* field = type.findField("rotation")) {
        if (const auto f = patch.fields.find(field->id);
            f != patch.fields.end()) {
            if (const Quat* q = std::get_if<Quat>(&f->second)) {
                transform.rotation = *q;
            }
        }
    }
    if (const reflect::FieldInfo* field = type.findField("scale")) {
        if (const auto f = patch.fields.find(field->id);
            f != patch.fields.end()) {
            if (const Vec3* s = std::get_if<Vec3>(&f->second)) {
                transform.scale = *s;
            }
        }
    }
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
        entry.abilities.clear(); // FOLLOWERS É6
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
            } else if (record.typeId ==
                       gameplay::SavedAbilityForm::staticTypeInfo().id) {
                entry.abilities.push_back( // FOLLOWERS É6
                    gameplay::formFromRecord<gameplay::SavedAbilityForm>(
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
        for (const auto& ability : entry.abilities) { // FOLLOWERS É6
            saved.abilities.push_back(&ability);
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

vector<SaveSlotInfo> listSaveSlots() {
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
    vector<SaveSlotInfo> infos;
    infos.reserve(slots.size());
    for (Slot& slot : slots) {
        str stamp;
        const auto system = std::chrono::clock_cast<std::chrono::system_clock>(
            slot.time);
        const std::time_t t = std::chrono::system_clock::to_time_t(system);
        // C9.8: platform::localTime — localtime_s is MSVC-only (glibc
        // has localtime_r with reversed arguments).
        const std::tm local = platform::localTime(t);
        char buffer[24];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
        stamp = buffer;
        infos.push_back({ std::move(slot.name), std::move(stamp) });
    }
    return infos;
}

str serializeSave(const data::Plugin& plugin,
                  const data::FormTypeRegistry& types) {
    // Pure — this is the seam the async save runs on a worker (C9.7).
    return data::writePluginToml(plugin, types);
}

bool writeSaveText(const str& slot, const str& text) {
    std::error_code ec;
    std::filesystem::create_directories(savesDirectory(), ec);
    const std::filesystem::path target = savePath(slot);
    std::filesystem::path tmp = target;
    tmp += ".tmp"; // saves/<slot>.toml.tmp
    std::ofstream out { tmp, std::ios::binary };
    if (!out) {
        LOG_ERROR("save: cannot write {}", tmp.string());
        return false;
    }
    out << text;
    out.close(); // flush BEFORE the publish — the rename is the commit
    if (out.fail()) {
        LOG_ERROR("save: write failed for {}", tmp.string());
        std::filesystem::remove(tmp, ec);
        return false;
    }
    // Atomic publish. std::filesystem::rename replaces an existing target
    // on POSIX; on Windows it can refuse when the target exists — fall
    // back to remove + rename (a crash between the two loses only the OLD
    // file while the new one is complete in .tmp).
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::error_code removeEc;
        std::filesystem::remove(target, removeEc);
        ec.clear();
        std::filesystem::rename(tmp, target, ec);
        if (ec) {
            LOG_ERROR("save: cannot publish {} ({})", target.string(),
                      ec.message());
            std::filesystem::remove(tmp, removeEc);
            return false;
        }
    }
    return true;
}

bool writeSave(const str& slot, const data::Plugin& plugin,
               const data::FormTypeRegistry& types) {
    if (!writeSaveText(slot, serializeSave(plugin, types))) {
        return false;
    }
    LOG_INFO("Saved: {} ({} records)", savePath(slot).string(),
             plugin.records.size());
    return true;
}

// --- SaveFlightGate (C9.7) ------------------------------------------------------------

bool SaveFlightGate::requestStart(const str& slot) {
    if (inFlight) {
        deferred = slot; // last wins — F5 spam never grows a queue
        return false;
    }
    inFlight = true;
    return true;
}

std::optional<str> SaveFlightGate::onComplete() {
    inFlight = false;
    std::optional<str> next;
    std::swap(next, deferred);
    return next;
}

std::optional<data::Plugin> readSave(const str& slot,
                                     const data::FormTypeRegistry& types) {
    std::ifstream in { savePath(slot), std::ios::binary };
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    auto parsed = data::parsePluginToml(text.str(), types, slot);
    if (!parsed) {
        // U1-03: a corrupt save now says WHY it will not load.
        LOG_ERROR("save '{}' unreadable: {}", slot, parsed.error());
        return std::nullopt;
    }
    return std::move(*parsed);
}

} // namespace game
