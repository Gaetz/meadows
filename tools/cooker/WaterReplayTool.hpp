#pragma once

#include <string>

#include "engine/terrain/WaterSim.hpp"

namespace cooker {

// Shared by the offline water benches (water-replay, water-bench).
struct SimStats {
    u32 wet { 0 };
    u32 pinned { 0 };
    u32 isolated { 0 };
    f64 volume { 0.0 };
    f32 maxDepth { 0.0f };
    f32 maxSpeed { 0.0f };
    // Brook census (cells under a ribbon too narrow to pin): how much
    // of the brook relief carries a film the extraction publishes
    // (8 mm on / 3 mm hold), and how thick.
    u32 brookCells { 0 };
    u32 brookOver8mm { 0 };
    u32 brookOver3mm { 0 };
    f32 brookMedian { 0.0f };
    f32 brookP90 { 0.0f };
    f32 brookMax { 0.0f };
};
SimStats stats(const render::terrain::WaterSimState& state,
               const render::terrain::WaterSimParams& params);
void printStats(const char* label, const SimStats& s);
// Judgment map: depth as blue, pinned cells marked, dry ground shaded.
void renderMap(const render::terrain::WaterSimState& state,
               const render::terrain::WaterSimParams& params,
               const std::string& path);
// One line per "x z" of `path` (a course's published points, pasted
// from landscape-report --at): ground, depth (3x3 max), surface, pin
// level, speed, brook flag at the nearest cell.
void printProbe(const render::terrain::WaterSimState& state,
                const render::terrain::WaterSimParams& params,
                const std::string& path, const char* label);

// `cooker water-replay <dump.wsd> <out-prefix> [substeps]` — load an
// in-game sim dump (Water panel > "Dump sim state"), print its stats,
// render a judgment map, optionally step it and render again. The
// offline reproduction bench for the live water sim.
int waterReplay(char** argv, int argc);

} // namespace cooker
