#include "world/ai/Pathfinding.hpp"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <queue>

namespace ai {

void Grid::setBlocked(i32 x, i32 y, bool value) {
    if (inBounds(x, y)) {
        blocked[static_cast<size_t>(y) * width + x] = value ? 1 : 0;
    }
}

bool Grid::isWalkable(i32 x, i32 y) const {
    return inBounds(x, y) && blocked[static_cast<size_t>(y) * width + x] == 0;
}

namespace {

i32 heuristic(GridCoord a, GridCoord b) {
    return std::abs(a.x - b.x) + std::abs(a.y - b.y);
}

} // namespace

vector<GridCoord> findPath(const Grid& grid, GridCoord start, GridCoord goal) {
    if (!grid.isWalkable(start.x, start.y) || !grid.isWalkable(goal.x, goal.y)) {
        return {};
    }
    const i32 width = grid.getWidth();
    const auto index = [width](GridCoord c) {
        return static_cast<size_t>(c.y) * width + c.x;
    };

    vector<i32> gScore(static_cast<size_t>(width) * grid.getHeight(), INT_MAX);
    vector<i32> cameFrom(gScore.size(), -1);

    struct Node {
        GridCoord coord;
        i32 f;
    };
    const auto worse = [](const Node& a, const Node& b) { return a.f > b.f; };
    std::priority_queue<Node, vector<Node>, decltype(worse)> open(worse);

    gScore[index(start)] = 0;
    open.push({ start, heuristic(start, goal) });

    constexpr GridCoord kNeighbors[4] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    while (!open.empty()) {
        const Node current = open.top();
        open.pop();

        if (current.coord == goal) {
            vector<GridCoord> path;
            for (i32 i = static_cast<i32>(index(goal)); i != -1; i = cameFrom[i]) {
                path.push_back({ i % width, i / width });
            }
            std::reverse(path.begin(), path.end());
            return path;
        }

        const i32 currentG = gScore[index(current.coord)];
        for (const GridCoord step : kNeighbors) {
            const GridCoord next { current.coord.x + step.x,
                                   current.coord.y + step.y };
            if (!grid.isWalkable(next.x, next.y)) {
                continue;
            }
            const i32 tentative = currentG + 1;
            if (tentative < gScore[index(next)]) {
                cameFrom[index(next)] = static_cast<i32>(index(current.coord));
                gScore[index(next)] = tentative;
                open.push({ next, tentative + heuristic(next, goal) });
            }
        }
    }
    return {};
}

} // namespace ai
