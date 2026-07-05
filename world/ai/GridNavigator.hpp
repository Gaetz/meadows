#pragma once

#include "engine/nav/Nav.hpp"
#include "world/ai/Pathfinding.hpp"

namespace world {

// The 2D-phase nav::Navigator: adapts the existing grid A* to the nav
// seam. World XZ (or XY in 2D scenes — the caller picks the plane via
// `cellSize` mapping done here on X/Y) maps onto grid cells; Y of the
// waypoints is left at 0 for the 2D phase.
class GridNavigator final : public nav::Navigator {
public:
    GridNavigator(const ai::Grid& grid, f32 cellSize)
        : grid { grid }, cellSize { cellSize } {}

    nav::PathResult findPath(const nav::PathQuery& query) const override;

private:
    const ai::Grid& grid;
    f32 cellSize;
};

} // namespace world
