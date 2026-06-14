#pragma once

#include "engine/core/Defines.hpp"

namespace ai {

struct GridCoord {
    i32 x { 0 };
    i32 y { 0 };
    bool operator==(const GridCoord&) const = default;
};

// A rectangular walkability grid (the 2D-phase navmesh stand-in; Recast/Detour
// arrives in 3D). Cells default to walkable.
class Grid {
public:
    Grid(i32 width, i32 height)
        : width(width), height(height),
          blocked(static_cast<size_t>(width) * height, 0) {}

    i32 getWidth() const { return width; }
    i32 getHeight() const { return height; }

    bool inBounds(i32 x, i32 y) const {
        return x >= 0 && y >= 0 && x < width && y < height;
    }
    void setBlocked(i32 x, i32 y, bool value);
    bool isWalkable(i32 x, i32 y) const;

private:
    i32 width;
    i32 height;
    vector<u8> blocked;
};

// 4-connected A* with a Manhattan heuristic. Returns the path from start to goal
// inclusive, or an empty path if unreachable (or either endpoint is blocked).
vector<GridCoord> findPath(const Grid& grid, GridCoord start, GridCoord goal);

} // namespace ai
