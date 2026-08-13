#include "world/ai/InteriorNavigator.hpp"

#include <algorithm>
#include <cmath>
#include <queue>

namespace world {

namespace {

struct OpenNode {
    f32 fScore;
    u32 level; // index into NavGrid::levels — doubles as the node id
    bool operator>(const OpenNode& other) const {
        return fScore > other.fScore;
    }
};

} // namespace

nav::PathResult InteriorNavigator::findPath(const nav::PathQuery& query) const {
    nav::PathResult result;
    if (grid.empty()) {
        return result;
    }

    // Snap an endpoint to the nearest walkable level of its column.
    const auto snap = [&](const Vec3& p) -> u32 {
        const u32 column = grid.columnOf(p.x, p.z);
        if (column == ~0u) {
            return ~0u;
        }
        u32 begin = 0;
        u32 end = 0;
        grid.columnLevels(column, begin, end);
        u32 best = ~0u;
        f32 bestDelta = snapRange;
        for (u32 l = begin; l < end; ++l) {
            const f32 delta = std::abs(grid.levels[l].floorY - p.y);
            if (delta < bestDelta) {
                bestDelta = delta;
                best = l;
            }
        }
        return best;
    };

    const u32 startLevel = snap(query.from);
    const u32 goalLevel = snap(query.to);
    if (startLevel == ~0u || goalLevel == ~0u) {
        return result;
    }

    // Level index -> column coordinates, resolved once (CSR is column-major
    // over z * width + x).
    const auto columnCoords = [&](u32 level, i32& ix, i32& iz) {
        // Binary search the CSR row starts for the owning column.
        const auto it = std::upper_bound(grid.firstLevel.begin(),
                                         grid.firstLevel.end(), level);
        const u32 column =
            static_cast<u32>(it - grid.firstLevel.begin()) - 1;
        ix = static_cast<i32>(column % grid.width);
        iz = static_cast<i32>(column / grid.width);
    };
    const auto worldOf = [&](u32 level) {
        i32 ix = 0;
        i32 iz = 0;
        columnCoords(level, ix, iz);
        return Vec3 { grid.originX +
                          (static_cast<f32>(ix) + 0.5f) * grid.cellSize,
                      grid.levels[level].floorY,
                      grid.originZ +
                          (static_cast<f32>(iz) + 0.5f) * grid.cellSize };
    };

    const Vec3 goalPos = worldOf(goalLevel);
    const auto heuristic = [&](u32 level) {
        return glm::length(worldOf(level) - goalPos);
    };

    vector<f32> gScore(grid.levels.size(), 1e30f);
    vector<u32> cameFrom(grid.levels.size(), ~0u);
    vector<bool> closed(grid.levels.size(), false);
    std::priority_queue<OpenNode, vector<OpenNode>, std::greater<OpenNode>>
        open;
    gScore[startLevel] = 0.0f;
    open.push({ heuristic(startLevel), startLevel });

    u32 expansions = 0;
    bool found = false;
    while (!open.empty() && expansions < maxExpansions) {
        const OpenNode node = open.top();
        open.pop();
        if (node.level == goalLevel) {
            found = true;
            break;
        }
        if (closed[node.level]) {
            continue; // stale queue entry (a better g was expanded already)
        }
        closed[node.level] = true;
        ++expansions;

        i32 ix = 0;
        i32 iz = 0;
        columnCoords(node.level, ix, iz);
        const f32 floorY = grid.levels[node.level].floorY;
        const i32 steps[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (const auto& s : steps) {
            const i32 nx = ix + s[0];
            const i32 nz = iz + s[1];
            if (nx < 0 || nz < 0 || nx >= static_cast<i32>(grid.width) ||
                nz >= static_cast<i32>(grid.depth)) {
                continue;
            }
            const u32 column =
                static_cast<u32>(nz) * grid.width + static_cast<u32>(nx);
            u32 begin = 0;
            u32 end = 0;
            grid.columnLevels(column, begin, end);
            for (u32 next = begin; next < end; ++next) {
                const f32 delta =
                    std::abs(grid.levels[next].floorY - floorY);
                if (delta > maxStep) {
                    continue;
                }
                const f32 cost = grid.cellSize + delta;
                if (gScore[node.level] + cost < gScore[next]) {
                    gScore[next] = gScore[node.level] + cost;
                    cameFrom[next] = node.level;
                    open.push({ gScore[next] + heuristic(next), next });
                }
            }
        }
    }
    if (!found) {
        return result;
    }

    vector<Vec3> reversed;
    for (u32 level = goalLevel; level != ~0u; level = cameFrom[level]) {
        reversed.push_back(worldOf(level));
        if (level == startLevel) {
            break;
        }
    }
    result.success = true;
    result.waypoints.push_back(query.from);
    for (auto it = reversed.rbegin(); it != reversed.rend(); ++it) {
        result.waypoints.push_back(*it);
    }
    result.waypoints.push_back(query.to);
    return result;
}

} // namespace world
