#include "world/streaming/CellLoader.hpp"

namespace world {

ecs::Entity CellLoader::loadCell(data::FormHandle cell) {
    if (const auto it = loaded.find(cell.value); it != loaded.end()) {
        return it->second;
    }

    ecs::Entity cellEntity = world.create();
    loaded.emplace(cell.value, cellEntity);

    SpawnContext ctx { world, forms, categories, spawnFilter };
    const u32 referenceTypeId = ReferenceForm::staticTypeInfo().id;
    for (const data::FormHandle handle : model.referencesIn(cell)) {
        const reflect::TypeInfo* type = forms.typeOf(handle);
        const data::Form* form = forms.get(handle);
        if (!type || !form || !type->isA(referenceTypeId)) {
            continue;
        }
        const auto* reference = static_cast<const ReferenceForm*>(form);
        if (!reference->enabled) {
            continue; // disabled references are not spawned (the loader's call)
        }
        if (reference->prefab.isValid()) {
            continue; // prefab TEMPLATE child: only spawned when a placed
                      // reference instantiates its PrefabForm (H8)
        }
        if (spawnFilter && !spawnFilter(reference->id)) {
            continue; // vetoed by the pending save layer (chantier 5)
        }
        spawner.spawn(ctx, *reference, cellEntity);
    }
    return cellEntity;
}

void CellLoader::unloadCell(data::FormHandle cell) {
    const auto it = loaded.find(cell.value);
    if (it == loaded.end()) {
        return;
    }
    ecs::Entity cellEntity = it->second;
    if (beforeUnload) {
        beforeUnload(cell, cellEntity); // capture while still alive
    }
    world.handle().delete_with<ecs::InCell>(cellEntity); // the cell's references
    cellEntity.destruct();
    loaded.erase(it);
}

void CellLoader::loadAll() {
    for (const data::FormHandle cell : model.cells()) {
        loadCell(cell);
    }
}

void CellLoader::unloadAll() {
    vector<data::FormHandle> cells;
    cells.reserve(loaded.size());
    for (const auto& [value, entity] : loaded) {
        cells.push_back(data::FormHandle { value });
    }
    for (const data::FormHandle cell : cells) {
        unloadCell(cell);
    }
}

ecs::Entity CellLoader::cellEntity(data::FormHandle cell) const {
    const auto it = loaded.find(cell.value);
    return it != loaded.end() ? it->second : ecs::Entity {};
}

} // namespace world
