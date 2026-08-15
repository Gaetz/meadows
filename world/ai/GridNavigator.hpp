#pragma once

#include "engine/nav/Nav.hpp"
#include "world/ai/Pathfinding.hpp"

namespace world {

// The 2D-phase nav::Navigator: adapts the existing grid A* to the nav
// seam. World X/Y (the 2D scenes' plane) map onto grid cells; waypoints
// come back as (x, y, 0).
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
