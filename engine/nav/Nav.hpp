#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

// The navigation seam (docs/HORIZONTAL-PASS.md) — INTERFACE ONLY by
// decision: Recast/Detour comes later; until then the 2D grid A*
// (world/ai/GridNavigator) implements this contract. Consumers (AI
// packages, schedules' travel, the player's companion someday) depend on
// nav::Navigator and never on the implementation.
//
// Planned extension points:
//  - RecastNavigator: navmesh baked per cell by the cooker, tiles swapped
//    on cell load/unload, findPath via Detour; off-mesh links for doors;
//  - make it async for long paths: same worker/queue pattern as
//    streaming (a PathRequest handle polled by the AI) — the interface
//    can grow a requestPath() then, keep findPath for short queries;
//  - agent radius/height parameters once characters vary in size.

namespace nav {

struct PathQuery {
    Vec3 from {};
    Vec3 to {};
    f32 acceptanceRadius { 0.5f }; // "close enough" to the goal
};

struct PathResult {
    bool success { false };
    vector<Vec3> waypoints; // from -> to inclusive when successful
};

class Navigator {
public:
    virtual ~Navigator() = default;
    virtual PathResult findPath(const PathQuery& query) const = 0;
};

} // namespace nav
