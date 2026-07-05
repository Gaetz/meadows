#include "world/ai/GridNavigator.hpp"

#include <cmath>

namespace world {

nav::PathResult GridNavigator::findPath(const nav::PathQuery& query) const {
    const auto toCell = [&](const Vec3& p) {
        return ai::GridCoord { static_cast<i32>(std::floor(p.x / cellSize)),
                               static_cast<i32>(
                                   std::floor(p.y / cellSize)) };
    };
    const vector<ai::GridCoord> cells =
        ai::findPath(grid, toCell(query.from), toCell(query.to));
    nav::PathResult result;
    if (cells.empty()) {
        return result;
    }
    result.success = true;
    result.waypoints.reserve(cells.size());
    for (const ai::GridCoord& cell : cells) {
        result.waypoints.push_back(
            { (static_cast<f32>(cell.x) + 0.5f) * cellSize,
              (static_cast<f32>(cell.y) + 0.5f) * cellSize, 0.0f });
    }
    return result;
}

} // namespace world
