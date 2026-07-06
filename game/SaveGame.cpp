#include "game/SaveGame.hpp"

#include "data/forms/FormDatabase.hpp"
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

} // namespace game
