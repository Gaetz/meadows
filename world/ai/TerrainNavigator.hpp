#pragma once

#include <functional>
#include <unordered_map>

#include "engine/nav/Nav.hpp"

// 3D terrain navigation — the SANCTIONED fallback of the
// nav seam: Recast/Detour remains the target implementation, this grid
// A* over the height function unblocks NPC life now.
//
// Walkability = slope under a threshold AND no blocking box. Blocking
// boxes are world-space AABBs the caller feeds from its static colliders
// (inflated by the agent radius). The grid is sampled LAZILY around the
// query (1 m cells, bounded search), so there is nothing to bake or
// invalidate — sculpting terrain or streaming cells just works.
//
// Headless-testable: the height source is a callback.

namespace world {

class TerrainNavigator final : public nav::Navigator {
public:
    using HeightFn = std::function<f32(f32 x, f32 z)>;

    struct BlockingBox {
        Vec3 min {};
        Vec3 max {};
    };

    explicit TerrainNavigator(HeightFn height) : height { std::move(height) } {}

    // Replaces the obstacle set (the scene refreshes it when cells
    // change). Boxes should already include the agent radius. Builds
    // the cell index blocked() queries — a box registers in every cell
    // its XZ rect touches, so any point inside it finds it.
    void setBlockingBoxes(vector<BlockingBox> boxes);

    // A* on the lazy 1 m grid; waypoints ride the terrain height.
    nav::PathResult findPath(const nav::PathQuery& query) const override;

    f32 maxSlope { 0.9f };       // max height delta per 1 m step
    u32 maxExpansions { 20000 }; // search budget (~140 m radius worst case)

private:
    bool blocked(f32 x, f32 z, f32 y) const;

    HeightFn height;
    vector<BlockingBox> blocking;
    // XZ cell -> indices of the boxes touching it: blocked() tests a
    // handful of candidates instead of every box per A* neighbor.
    static constexpr f32 kBlockCell = 16.0f;
    std::unordered_map<u64, vector<u32>> blockIndex;
};

} // namespace world
