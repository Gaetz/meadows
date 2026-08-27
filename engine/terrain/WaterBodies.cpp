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
        // A MASKED lake's basin mask is the authority: any depth inside
        // it is genuinely underwater (the below-slack heuristic exists
        // for hand-authored bbox ponds over galleries — applied to
        // generated lakes it read the bottom of a deep basin as DRY,
        // and spawned players and trees 70 m under the surface).
        const bool inReach =
            lake.mask.empty()
                ? plausible(lake.level, probeY)
                : probeY < lake.level + kAboveSlack;
        if (inReach && (!best || lake.level > *best)) {
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

RiverFlowSample riverFlowSample(const RiverSurface& river, f32 x, f32 z) {
    RiverFlowSample out;
    if (x < river.minX || x > river.maxX || z < river.minZ ||
        z > river.maxZ) {
        return out;
    }
    f32 bestLat = 1.0f;
    for (size_t i = 0; i + 1 < river.nodes.size(); ++i) {
        const RiverNode& a = river.nodes[i];
        const RiverNode& b = river.nodes[i + 1];
        const f32 abx = b.x - a.x;
        const f32 abz = b.z - a.z;
        const f32 len2 = abx * abx + abz * abz;
        if (len2 <= 0.0f) {
            continue;
        }
        const f32 t = glm::clamp(
            ((x - a.x) * abx + (z - a.z) * abz) / len2, 0.0f, 1.0f);
        const f32 px = a.x + abx * t;
        const f32 pz = a.z + abz * t;
        const f32 dist =
            std::sqrt((x - px) * (x - px) + (z - pz) * (z - pz));
        const f32 half = glm::mix(a.halfWidth, b.halfWidth, t);
        if (half <= 0.0f || dist > half) {
            continue;
        }
        const f32 lat = dist / half;
        if (out.weight > 0.0f && lat >= bestLat) {
            continue;
        }
        bestLat = lat;
        const f32 len = std::sqrt(len2);
        // Mid-channel outruns the banks — the same lateral profile the
        // water shader advects with (0.55 + 0.45 * (1 - lat^2)).
        const f32 profile = 0.55f + 0.45f * (1.0f - lat * lat);
        out.flow = Vec2 { abx / len, abz / len } * river.flowSpeed *
                   profile;
        out.weight = 1.0f - lat;
        out.surface = glm::mix(a.surface, b.surface, t);
    }
    return out;
}

Vec2 waterFlowAt(const WaterBodies& bodies, f32 x, f32 z, f32 probeY) {
    Vec2 sum { 0.0f, 0.0f };
    f32 weightSum = 0.0f;
    for (const RiverSurface& river : bodies.rivers) {
        const RiverFlowSample sample = riverFlowSample(river, x, z);
        if (sample.weight <= 0.0f ||
            !plausible(sample.surface, probeY)) {
            continue;
        }
        sum += sample.flow * sample.weight;
        weightSum += sample.weight;
    }
    return weightSum > 0.0f ? sum / weightSum : Vec2 { 0.0f, 0.0f };
}

f32 waterDepthAt(const WaterBodies& bodies, f32 x, f32 z, f32 terrainY) {
    const auto surface = waterSurfaceAt(bodies, x, z, terrainY);
    return surface ? glm::max(*surface - terrainY, 0.0f) : 0.0f;
}

} // namespace render::terrain
