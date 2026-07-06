#include "game/SaveGame.hpp"

#include <algorithm>

#include "data/forms/FormDatabase.hpp"
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

} // namespace game
