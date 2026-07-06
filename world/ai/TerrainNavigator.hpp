#pragma once

#include <functional>

#include "engine/nav/Nav.hpp"

// 3D terrain navigation (chantier 3 B2 — the SANCTIONED fallback of the
// nav seam: Recast/Detour remains the target implementation, this grid
// A* over the height function unblocks the « vivant » vertical now).
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
    // change). Boxes should already include the agent radius.
    void setBlockingBoxes(vector<BlockingBox> boxes) {
        blocking = std::move(boxes);
    }

    // A* on the lazy 1 m grid; waypoints ride the terrain height.
    nav::PathResult findPath(const nav::PathQuery& query) const override;

    f32 maxSlope { 0.9f };       // max height delta per 1 m step
    u32 maxExpansions { 20000 }; // search budget (~140 m radius worst case)

private:
    bool blocked(f32 x, f32 z, f32 y) const;

    HeightFn height;
    vector<BlockingBox> blocking;
};

} // namespace world
