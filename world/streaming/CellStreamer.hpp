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
    //
    // `maxLoads` (chantier 5 B8, the smoothed-streaming v1): 0 = load the
    // whole ring now (scene enter, travels — behind a fade); N = load at
    // most N cells per call and keep the ring marked incomplete, so the
    // next call resumes — border crossings spread their spawns over
    // frames instead of hitching.
    bool update(data::FormHandle worldspace, f32 focusX, f32 focusZ,
                i32 loadRadius = 2, i32 unloadRadius = 3, u32 maxLoads = 0);

    // Editor "load this cell NOW" (IMPLICIT-CELLS brick 2): loads `cell`
    // through the loader and marks it resident, so the next ring update
    // never double-loads it. From then on it is an ordinary resident —
    // hysteresis evicts it when the focus moves away. No-op when already
    // resident or the handle is invalid.
    void adopt(data::FormHandle cell);

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
