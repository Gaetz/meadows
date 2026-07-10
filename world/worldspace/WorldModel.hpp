#pragma once

#include <map>
#include <tuple>
#include <unordered_map>

#include "data/forms/FormDatabase.hpp"

namespace world {

// The resolved spatial index over a FormDatabase: what FormDatabase is to
// definitions, WorldModel is to space. Read-only, rebuilt after each §5
// resolution (cheap; mirrors how the game re-resolves on a live mod toggle).
// Deterministic: it scans forms in handle order, so reference lists are stable.
//
// Holds no runtime/ECS state — purely an index of Forms. The cell-loader (brick
// e) consumes it to spawn entities; nothing here depends on flecs.
class WorldModel {
public:
    static WorldModel build(const data::FormDatabase& forms);

    // The cell at integer grid coords within a worldspace; invalid handle if
    // none exists.
    data::FormHandle cellAt(data::FormHandle worldspace, i32 x, i32 y) const;

    // Implicit cells (chantier IMPLICIT-CELLS, brick 1): materializes the
    // (gx, gy) square of `worldspace`. Idempotent — returns the existing
    // handle when the square already has a CellForm (authored or
    // previously materialized); otherwise creates one LIVE in `forms`
    // under its deterministic guid (cellGuidFor, §2.5) and indexes it
    // here. Interior flag inherited from the worldspace; the streamer
    // needs no change (cellAt simply starts resolving). Worldspace-
    // agnostic: exteriors get "edit anywhere", interiors can grow beyond
    // their authored rooms. Invalid handle if `worldspace` is not one.
    data::FormHandle materializeCell(data::FormDatabase& forms,
                                     data::FormHandle worldspace, i32 gx,
                                     i32 gy);

    // References placed in a cell, in deterministic (handle) order. Includes
    // disabled references (ReferenceForm.enabled == false): filtering is the
    // loader's call. Empty list for an unknown or childless cell.
    const vector<data::FormHandle>& referencesIn(data::FormHandle cell) const;

    // The worldspace a cell belongs to; invalid handle if unknown.
    data::FormHandle worldspaceOf(data::FormHandle cell) const;

    const vector<data::FormHandle>& worldspaces() const { return allWorldspaces; }
    const vector<data::FormHandle>& cells() const { return allCells; }

private:
    vector<data::FormHandle> allWorldspaces;
    vector<data::FormHandle> allCells;
    // key = (worldspace handle value, gridX, gridY)
    std::map<std::tuple<u32, i32, i32>, data::FormHandle> cellByCoords;
    std::unordered_map<u32, data::FormHandle> worldspaceByCell;          // cell value -> worldspace
    std::unordered_map<u32, vector<data::FormHandle>> referencesByCell;  // cell value -> refs
};

} // namespace world
