#pragma once

#include <optional>

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

// Local water bodies: the sea plus altitude lakes (flat surfaces at their
// own level) and rivers (polyline ribbons with a monotone-downhill
// surface). Headless home (HeightPatches rationale): gameplay queries
// swim/wading through the pure functions below, the render side draws the
// same data. Built from WaterBodyForm/RiverForm records
// (world/terrain/WaterBodiesBuilder) and from sandbox tile bakes.

namespace render {

struct LakeSurface {
    f32 level { 0.0f }; // water surface, meters
    // Footprint bounds; the terrain basin clips the shoreline via the
    // depth test, exactly like the sea plane does.
    f32 minX { 0.0f };
    f32 minZ { 0.0f };
    f32 maxX { 0.0f };
    f32 maxZ { 0.0f };
    Vec3 tint { 0.10f, 0.30f, 0.34f };
    f32 chop { 0.5f };
    // Bbox-local basin mask (1 = water). Empty = the whole bbox counts
    // (hand-authored rectangular ponds); generated lakes always carry it
    // — a bbox rectangle at lake level floats over anything lower inside
    // the bounds.
    u32 maskWidth { 0 };
    u32 maskHeight { 0 };
    f32 maskTexel { 8.0f };
    vector<u8> mask;

    bool covers(f32 x, f32 z) const {
        if (x < minX || x > maxX || z < minZ || z > maxZ) {
            return false;
        }
        if (mask.empty() || maskWidth == 0 || maskHeight == 0) {
            return true;
        }
        const u32 mx = glm::min(
            static_cast<u32>(glm::max((x - minX) / maskTexel + 0.5f,
                                      0.0f)),
            maskWidth - 1);
        const u32 mz = glm::min(
            static_cast<u32>(glm::max((z - minZ) / maskTexel + 0.5f,
                                      0.0f)),
            maskHeight - 1);
        return mask[static_cast<size_t>(mz) * maskWidth + mx] != 0;
    }
};

struct RiverNode {
    f32 x { 0.0f };
    f32 z { 0.0f };
    f32 surface { 0.0f };   // water level at this node (monotone downhill
                            // along the polyline — the bake's contract)
    f32 halfWidth { 0.0f }; // meters
};

struct RiverSurface {
    vector<RiverNode> nodes; // downstream order
    Vec3 tint { 0.10f, 0.30f, 0.34f };
    f32 flowSpeed { 1.0f };
    // Bounds incl. widths (kept for the point queries).
    f32 minX { 0.0f };
    f32 minZ { 0.0f };
    f32 maxX { 0.0f };
    f32 maxZ { 0.0f };
};

struct WaterBodies {
    f32 seaLevel { 21.0f };
    vector<LakeSurface> lakes;
    vector<RiverSurface> rivers;
};

namespace terrain {

// The highest plausible water surface over (x, z) for a probe at probeY:
// the sea (when the probe is not far above it), a covering lake whose
// level is near/above the probe (a probe under a cliff below a lake's
// bbox does NOT count), or a river ribbon overlapping the point. nullopt
// = dry. The swim controller feeds the player position as probeY.
std::optional<f32> waterSurfaceAt(const WaterBodies& bodies, f32 x, f32 z,
                                  f32 probeY);

// Vertical water column over terrain at height terrainY (0 = dry) — the
// pool-depth/foam bake and wading effects.
f32 waterDepthAt(const WaterBodies& bodies, f32 x, f32 z, f32 terrainY);

} // namespace terrain

} // namespace render
