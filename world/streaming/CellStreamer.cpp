#include "world/streaming/CellStreamer.hpp"

#include <cmath>

#include "world/worldspace/WorldForms.hpp"

namespace world {

bool CellStreamer::update(data::FormHandle worldspace, f32 focusX,
                          f32 focusZ, i32 loadRadius, i32 unloadRadius) {
    const reflect::TypeInfo* spaceType = forms.typeOf(worldspace);
    if (!spaceType ||
        !spaceType->isA(WorldspaceForm::staticTypeInfo().id)) {
        return false;
    }
    const auto* space = static_cast<const WorldspaceForm*>(
        forms.get(worldspace));
    if (!space || space->cellSize <= 0.0f) {
        return false;
    }
    const i32 centerX =
        static_cast<i32>(std::floor(focusX / space->cellSize));
    const i32 centerY =
        static_cast<i32>(std::floor(focusZ / space->cellSize));
    if (ringValid && lastWorldspace == worldspace.value &&
        centerX == lastCenterX && centerY == lastCenterY) {
        return false; // same center cell -> the ring cannot have changed
    }
    lastCenterX = centerX;
    lastCenterY = centerY;
    lastWorldspace = worldspace.value;
    ringValid = true;

    bool changed = false;
    // Load the ring. Cells that have no CellForm record simply don't
    // exist (hand-made world: authored cells only).
    for (i32 y = centerY - loadRadius; y <= centerY + loadRadius; ++y) {
        for (i32 x = centerX - loadRadius; x <= centerX + loadRadius; ++x) {
            const data::FormHandle cell = model.cellAt(worldspace, x, y);
            if (!cell.isValid() || resident.contains(cell.value)) {
                continue;
            }
            loader.loadCell(cell);
            resident.insert(cell.value);
            changed = true;
        }
    }
    // Evict beyond the hysteresis ring.
    for (auto it = resident.begin(); it != resident.end();) {
        const data::FormHandle cell { *it };
        const auto* form =
            static_cast<const CellForm*>(forms.get(cell));
        const bool tooFar =
            !form ||
            std::max(std::abs(form->gridX - centerX),
                     std::abs(form->gridY - centerY)) > unloadRadius;
        if (tooFar) {
            loader.unloadCell(cell);
            it = resident.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

void CellStreamer::unloadAll() {
    for (const u32 value : resident) {
        loader.unloadCell(data::FormHandle { value });
    }
    resident.clear();
    ringValid = false; // the next update() must rebuild the ring
}

} // namespace world
