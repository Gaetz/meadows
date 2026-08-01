#include "engine/terrain/WaterBodies.hpp"

#include <cmath>

namespace render::terrain {

namespace {

// A probe counts as "in reach" of a surface when it is not floating far
// above it (you swim IN water, not over it) and not buried impossibly
// deep under it (a gallery under a lake stays dry).
constexpr f32 kAboveSlack = 2.0f;
constexpr f32 kBelowSlack = 60.0f;

bool plausible(f32 level, f32 probeY) {
    return probeY < level + kAboveSlack && probeY > level - kBelowSlack;
}

std::optional<f32> riverSurface(const RiverSurface& river, f32 x, f32 z,
                                f32 probeY) {
    if (x < river.minX || x > river.maxX || z < river.minZ ||
        z > river.maxZ) {
        return std::nullopt;
    }
    std::optional<f32> best;
    for (size_t i = 0; i + 1 < river.nodes.size(); ++i) {
        const RiverNode& a = river.nodes[i];
        const RiverNode& b = river.nodes[i + 1];
        const f32 abx = b.x - a.x;
        const f32 abz = b.z - a.z;
        const f32 len2 = abx * abx + abz * abz;
        const f32 t =
            len2 > 0.0f
                ? glm::clamp(((x - a.x) * abx + (z - a.z) * abz) / len2,
                             0.0f, 1.0f)
                : 0.0f;
        const f32 px = a.x + abx * t;
        const f32 pz = a.z + abz * t;
        const f32 dist =
            std::sqrt((x - px) * (x - px) + (z - pz) * (z - pz));
        const f32 half = glm::mix(a.halfWidth, b.halfWidth, t);
        if (dist > half) {
            continue;
        }
        const f32 level = glm::mix(a.surface, b.surface, t);
        if (plausible(level, probeY) && (!best || level > *best)) {
            best = level;
        }
    }
    return best;
}

} // namespace

std::optional<f32> waterSurfaceAt(const WaterBodies& bodies, f32 x, f32 z,
                                  f32 probeY) {
    std::optional<f32> best;
    if (probeY < bodies.seaLevel + kAboveSlack) {
        best = bodies.seaLevel;
    }
    for (const LakeSurface& lake : bodies.lakes) {
        if (!lake.covers(x, z)) {
            continue;
        }
        if (plausible(lake.level, probeY) &&
            (!best || lake.level > *best)) {
            best = lake.level;
        }
    }
    for (const RiverSurface& river : bodies.rivers) {
        const auto level = riverSurface(river, x, z, probeY);
        if (level && (!best || *level > *best)) {
            best = level;
        }
    }
    return best;
}

f32 waterDepthAt(const WaterBodies& bodies, f32 x, f32 z, f32 terrainY) {
    const auto surface = waterSurfaceAt(bodies, x, z, terrainY);
    return surface ? glm::max(*surface - terrainY, 0.0f) : 0.0f;
}

} // namespace render::terrain
