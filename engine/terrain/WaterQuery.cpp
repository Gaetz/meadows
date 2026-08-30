#include "engine/terrain/WaterQuery.hpp"

#include <glm/glm.hpp>

namespace render::terrain {

namespace {

// Inside the sim's AUTHORITY zone? The trusted rect inset by a couple
// of texels — at the very band the render hands over to the baked, so
// does the query (max semantics: neither side can drop water there).
bool simAuthority(const WaterSimSnapshot& snap, f32 x, f32 z) {
    const auto& spec = snap.spec;
    if (spec.n < 2 || snap.depth.size() != spec.cells()) {
        return false;
    }
    const f32 inset =
        (static_cast<f32>(snap.marginCells) + 2.0f) * spec.texelSize;
    const f32 span = static_cast<f32>(spec.n - 1) * spec.texelSize;
    return x >= spec.originX + inset && x <= spec.originX + span - inset &&
           z >= spec.originZ + inset && z <= spec.originZ + span - inset;
}

// The sim sample counts for a probe near its column: falling into it
// (a few meters above the surface) or inside it — but a probe well
// BELOW the column's bottom is in a gallery under the water: dry.
bool plausible(const WaterSimSample& s, f32 probeY) {
    return probeY <= s.surface + 3.0f &&
           probeY >= (s.surface - s.depth) - 2.0f;
}

} // namespace

std::optional<f32> waterSurfaceQuery(const WaterQuery& q, f32 x, f32 z,
                                     f32 probeY) {
    if (q.sim != nullptr && simAuthority(*q.sim, x, z)) {
        const WaterSimSample s = sampleSnapshot(*q.sim, x, z);
        if (s.depth > 0.0f && plausible(s, probeY)) {
            return s.surface;
        }
        // Authoritatively dry — except the sea, which the sim
        // publishes dry by doctrine (the analytic sheet owns it).
        if (probeY < q.seaLevel + 2.0f) {
            return q.seaLevel;
        }
        return std::nullopt;
    }
    std::optional<f32> best;
    if (q.bodies != nullptr) {
        best = waterSurfaceAt(*q.bodies, x, z, probeY);
    }
    // Sea belt: some callers reach here without bodies (early frames).
    if (!best && probeY < q.seaLevel + 2.0f) {
        best = q.seaLevel;
    }
    return best;
}

Vec2 waterFlowQuery(const WaterQuery& q, f32 x, f32 z, f32 probeY) {
    if (q.sim != nullptr && simAuthority(*q.sim, x, z)) {
        const WaterSimSample s = sampleSnapshot(*q.sim, x, z);
        if (s.depth > 0.0f && plausible(s, probeY)) {
            return Vec2 { s.velocityX, s.velocityZ };
        }
        return Vec2 { 0.0f };
    }
    if (q.bodies != nullptr) {
        return waterFlowAt(*q.bodies, x, z, probeY);
    }
    return Vec2 { 0.0f };
}

} // namespace render::terrain
