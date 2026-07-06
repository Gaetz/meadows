#pragma once

#include <unordered_set>

#include "world/streaming/CellLoader.hpp"

namespace world {

// Distance-driven cell residency (chantier 2, B1): keeps the cells around
// a focus point loaded, with hysteresis so strolling along a border never
// churns — the TerrainCollision ring pattern applied to cells. SYNCHRONOUS
// by design: async streaming + per-cell persistence is the « persistance »
// chantier; do not anticipate it here.
//
// Grid convention: CellForm.gridX = floor(x / cellSize),
// CellForm.gridY = floor(z / cellSize) (the 2D-era "Y" is the Z axis).
// References with no cell (cell = 0) are PERSISTENT — never touched by
// the streamer; the scene spawns them once at startup (e.g. the player).
class CellStreamer {
public:
    CellStreamer(CellLoader& loader, const WorldModel& model,
                 const data::FormDatabase& forms)
        : loader { loader }, model { model }, forms { forms } {}

    // Ensures every cell of `worldspace` within `loadRadius` (Chebyshev,
    // in cells) of the focus is loaded, and unloads loaded cells beyond
    // `unloadRadius`. Returns true if any cell was loaded or unloaded —
    // the caller re-runs its post-spawn fixups (ground snap, NPC build)
    // only on change.
    bool update(data::FormHandle worldspace, f32 focusX, f32 focusZ,
                i32 loadRadius = 2, i32 unloadRadius = 3);

    // Unloads everything this streamer loaded (worldspace transitions).
    void unloadAll();

    u32 loadedCount() const { return static_cast<u32>(resident.size()); }

private:
    CellLoader& loader;
    const WorldModel& model;
    const data::FormDatabase& forms;
    std::unordered_set<u32> resident; // cell handle values we loaded
    // The ring can only change when the focus crosses a cell border (or
    // after unloadAll): skip the per-frame lookups otherwise.
    i32 lastCenterX { 0 };
    i32 lastCenterY { 0 };
    u32 lastWorldspace { 0 };
    bool ringValid { false };
};

} // namespace world
