#include "world/worldspace/WorldModel.hpp"

#include "engine/core/Log.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace world {

WorldModel WorldModel::build(const data::FormDatabase& forms) {
    WorldModel model;

    const u32 worldspaceTypeId = WorldspaceForm::staticTypeInfo().id;
    const u32 cellTypeId = CellForm::staticTypeInfo().id;
    const u32 referenceTypeId = ReferenceForm::staticTypeInfo().id;

    // FormHandle::value is index + 1; scan in handle order for determinism.
    for (u32 value = 1; value <= forms.count(); ++value) {
        const data::FormHandle handle { value };
        const data::Form* form = forms.get(handle);
        const reflect::TypeInfo* type = forms.typeOf(handle);
        if (!form || !type) {
            continue;
        }

        if (type->isA(worldspaceTypeId)) {
            model.allWorldspaces.push_back(handle);
        } else if (type->isA(cellTypeId)) {
            const auto* cell = static_cast<const CellForm*>(form);
            model.allCells.push_back(handle);
            const data::FormHandle worldspace = forms.handleOf(cell->worldspace);
            model.worldspaceByCell.emplace(handle.value, worldspace);
            model.cellByCoords.emplace(
                std::make_tuple(worldspace.value, cell->gridX, cell->gridY),
                handle);
        } else if (type->isA(referenceTypeId)) {
            const auto* reference = static_cast<const ReferenceForm*>(form);
            // No cell at all = a PERSISTENT reference (the player): the
            // scene spawns it once, the streamer never touches it. Only a
            // cell guid that fails to RESOLVE deserves the warning.
            if (!reference->cell.isValid()) {
                continue;
            }
            const data::FormHandle cell = forms.handleOf(reference->cell);
            if (!cell.isValid()) {
                LOG_WARN("WorldModel: reference {} placed in unknown cell {}",
                         reference->id.toString(),
                         reference->cell.toString());
                continue;
            }
            model.referencesByCell[cell.value].push_back(handle);
        }
    }

    return model;
}

data::FormHandle WorldModel::cellAt(data::FormHandle worldspace, i32 x,
                                    i32 y) const {
    const auto it = cellByCoords.find(std::make_tuple(worldspace.value, x, y));
    return it != cellByCoords.end() ? it->second : data::FormHandle {};
}

data::FormHandle WorldModel::materializeCell(data::FormDatabase& forms,
                                             data::FormHandle worldspace,
                                             i32 gx, i32 gy) {
    // Already authored/materialized: the index answers.
    if (const data::FormHandle existing = cellAt(worldspace, gx, gy);
        existing.isValid()) {
        return existing;
    }
    const auto* space =
        static_cast<const WorldspaceForm*>(forms.get(worldspace));
    const reflect::TypeInfo* spaceType = forms.typeOf(worldspace);
    if (!space || !spaceType ||
        !spaceType->isA(WorldspaceForm::staticTypeInfo().id)) {
        return {};
    }
    // A plugin may carry this square under its deterministic guid while
    // the index was built before (paranoia: build() would have indexed
    // it) — handleOf keeps materialize idempotent across that edge too.
    const core::Guid guid = cellGuidFor(space->id, gx, gy);
    if (const data::FormHandle resolved = forms.handleOf(guid);
        resolved.isValid()) {
        cellByCoords.emplace(std::make_tuple(worldspace.value, gx, gy),
                             resolved);
        worldspaceByCell.emplace(resolved.value, worldspace);
        return resolved;
    }

    auto cell = std::make_unique<CellForm>();
    cell->id = guid;
    cell->editorId = "cell_" + std::to_string(gx) + "_" +
                     std::to_string(gy); // authoring aid, not identity
    cell->worldspace = space->id;
    cell->gridX = gx;
    cell->gridY = gy;
    cell->interior = space->interior;
    const data::FormHandle handle =
        forms.add(std::move(cell), CellForm::staticTypeInfo());
    allCells.push_back(handle);
    cellByCoords.emplace(std::make_tuple(worldspace.value, gx, gy), handle);
    worldspaceByCell.emplace(handle.value, worldspace);
    return handle;
}

const vector<data::FormHandle>& WorldModel::referencesIn(
    data::FormHandle cell) const {
    static const vector<data::FormHandle> kEmpty;
    const auto it = referencesByCell.find(cell.value);
    return it != referencesByCell.end() ? it->second : kEmpty;
}

data::FormHandle WorldModel::worldspaceOf(data::FormHandle cell) const {
    const auto it = worldspaceByCell.find(cell.value);
    return it != worldspaceByCell.end() ? it->second : data::FormHandle {};
}

} // namespace world
