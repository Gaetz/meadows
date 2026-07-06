#include "world/ai/TerrainNavigator.hpp"

#include <cmath>
#include <queue>
#include <unordered_map>

namespace world {

namespace {

u64 keyOf(i32 x, i32 z) {
    return (static_cast<u64>(static_cast<u32>(x)) << 32) |
           static_cast<u64>(static_cast<u32>(z));
}

} // namespace

bool TerrainNavigator::blocked(f32 x, f32 z, f32 y) const {
    for (const BlockingBox& box : blocking) {
        if (x >= box.min.x && x <= box.max.x && z >= box.min.z &&
            z <= box.max.z && y >= box.min.y - 0.5f && y <= box.max.y) {
            return true;
        }
    }
    return false;
}

nav::PathResult TerrainNavigator::findPath(
    const nav::PathQuery& query) const {
    nav::PathResult result;
    const i32 startX = static_cast<i32>(std::round(query.from.x));
    const i32 startZ = static_cast<i32>(std::round(query.from.z));
    const i32 goalX = static_cast<i32>(std::round(query.to.x));
    const i32 goalZ = static_cast<i32>(std::round(query.to.z));

    struct Node {
        f32 fScore;
        i32 x, z;
        bool operator>(const Node& other) const {
            return fScore > other.fScore;
        }
    };
    const auto heuristic = [&](i32 x, i32 z) {
        const f32 dx = static_cast<f32>(x - goalX);
        const f32 dz = static_cast<f32>(z - goalZ);
        return std::sqrt(dx * dx + dz * dz);
    };

    std::priority_queue<Node, vector<Node>, std::greater<Node>> open;
    std::unordered_map<u64, f32> gScore;
    std::unordered_map<u64, u64> cameFrom;
    open.push({ heuristic(startX, startZ), startX, startZ });
    gScore[keyOf(startX, startZ)] = 0.0f;

    const f32 acceptance = glm::max(query.acceptanceRadius, 1.0f);
    u32 expansions = 0;
    u64 reached = 0;
    bool found = false;
    while (!open.empty() && expansions++ < maxExpansions) {
        const Node node = open.top();
        open.pop();
        if (heuristic(node.x, node.z) <= acceptance) {
            reached = keyOf(node.x, node.z);
            found = true;
            break;
        }
        const f32 nodeY = height(static_cast<f32>(node.x),
                                 static_cast<f32>(node.z));
        const f32 nodeG = gScore[keyOf(node.x, node.z)];
        constexpr i32 kSteps[8][2] = { { 1, 0 },  { -1, 0 }, { 0, 1 },
                                       { 0, -1 }, { 1, 1 },  { 1, -1 },
                                       { -1, 1 }, { -1, -1 } };
        for (const auto& step : kSteps) {
            const i32 nx = node.x + step[0];
            const i32 nz = node.z + step[1];
            const f32 stepLength =
                (step[0] != 0 && step[1] != 0) ? 1.41421f : 1.0f;
            const f32 ny =
                height(static_cast<f32>(nx), static_cast<f32>(nz));
            if (std::abs(ny - nodeY) > maxSlope * stepLength ||
                blocked(static_cast<f32>(nx), static_cast<f32>(nz), ny)) {
                continue;
            }
            const u64 nkey = keyOf(nx, nz);
            const f32 tentative = nodeG + stepLength;
            const auto it = gScore.find(nkey);
            if (it == gScore.end() || tentative < it->second) {
                gScore[nkey] = tentative;
                cameFrom[nkey] = keyOf(node.x, node.z);
                open.push({ tentative + heuristic(nx, nz), nx, nz });
            }
        }
    }
    if (!found) {
        return result;
    }

    // Walk back, then decimate: keep every 3rd waypoint (the mover steers
    // smoothly between them anyway), always keep the exact endpoints.
    vector<Vec3> reversed;
    u64 cursor = reached;
    const u64 startKey = keyOf(startX, startZ);
    while (true) {
        const i32 x = static_cast<i32>(cursor >> 32);
        const i32 z = static_cast<i32>(cursor & 0xffffffffu);
        reversed.push_back({ static_cast<f32>(x),
                             height(static_cast<f32>(x),
                                    static_cast<f32>(z)),
                             static_cast<f32>(z) });
        if (cursor == startKey) {
            break;
        }
        const auto it = cameFrom.find(cursor);
        if (it == cameFrom.end()) {
            break;
        }
        cursor = it->second;
    }
    result.success = true;
    result.waypoints.push_back(query.from);
    for (size_t i = reversed.size(); i-- > 0;) {
        const size_t fromEnd = reversed.size() - 1 - i;
        if (fromEnd % 3 == 0 || i == 0) {
            result.waypoints.push_back(reversed[i]);
        }
    }
    result.waypoints.push_back(
        { query.to.x, height(query.to.x, query.to.z), query.to.z });
    return result;
}

} // namespace world
