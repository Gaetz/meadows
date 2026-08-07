#pragma once

#include "engine/dungeon/NavGrid.hpp"
#include "engine/nav/Nav.hpp"

// Multi-level interior navigation — the nav seam's answer to dungeons.
// TerrainNavigator's height function cannot express two floors over the same
// XZ; this navigator runs A* on a baked dungeon::NavGrid where one column
// carries several walkable levels. A step between neighbouring columns is
// legal when the floor delta stays under maxStep — carved ramps qualify,
// vertical shafts do not, which is exactly what makes a drop one-way.
//
// Headless-testable like its sibling: the grid is plain data.

namespace world {

class InteriorNavigator final : public nav::Navigator {
public:
    explicit InteriorNavigator(dungeon::NavGrid navGrid)
        : grid { std::move(navGrid) } {}

    nav::PathResult findPath(const nav::PathQuery& query) const override;

    f32 maxStep { 0.55f };       // max floor delta between adjacent columns
    f32 snapRange { 2.5f };      // how far a query point may sit off a floor
    // Search budget: TerrainNavigator's 20k covers ~140 m at 1 m cells;
    // this grid is 0.5 m AND multi-level, so the same reach costs more.
    u32 maxExpansions { 80000 };

private:
    dungeon::NavGrid grid;
};

} // namespace world
