#include "engine/dungeon/DensityField.hpp"

#include <algorithm>

#include "engine/terrain/Noise.hpp"

namespace dungeon {

namespace {

// Approximate ellipsoid signed distance: exact at the surface normal
// directions, slightly conservative elsewhere — fine for carving.
f32 sdEllipsoid(const Vec3& p, const Vec3& center, const Vec3& radii) {
    const Vec3 q = (p - center) / radii;
    const f32 k0 = glm::length(q);
    if (k0 <= 0.0001f) {
        return -glm::min(radii.x, glm::min(radii.y, radii.z));
    }
    const f32 k1 = glm::length(q / radii);
    return k0 * (k0 - 1.0f) / k1;
}

f32 sdCapsule(const Vec3& p, const Vec3& a, const Vec3& b, f32 radius) {
    const Vec3 pa = p - a;
    const Vec3 ba = b - a;
    const f32 h =
        glm::clamp(glm::dot(pa, ba) / glm::dot(ba, ba), 0.0f, 1.0f);
    return glm::length(pa - ba * h) - radius;
}

} // namespace

DensityField::DensityField(const SpaceGraph& graph,
                           const DensityParams& fieldParams)
    : params(fieldParams) {
    const SpaceParams& sp = graph.params;

    for (const SpaceRoom& room : graph.rooms) {
        Ball ball;
        // The ellipsoid center rides LOW (same rule as the tunnels): the
        // flat floor disc spans r*sqrt(1 - liftFactor^2) of the radius —
        // at 0.85 a room's usable floor was HALF its nominal size and the
        // scattered props sat inside the curved wall base. At 0.45 the
        // disc is ~90% of r and dips a safe margin under the floor plane.
        const f32 span = static_cast<f32>(room.floorSpan - 1) * sp.floorSpacing;
        const f32 halfHeight = (params.roomHeight + span) * 0.5f;
        const Vec3 base = slotCenter(sp, room.pos);
        ball.center = { base.x, base.y + halfHeight * 0.45f, base.z };
        ball.radii = { room.radius, halfHeight, room.radius };
        ball.floorY = base.y;
        balls.push_back(ball);
    }

    // Corridor centerlines ride LOW above the slot floor: the tube must
    // keep dipping under the floor plane by more than the wall noise
    // (radius - lift > amplitude + margin), or the noise lifts the tube
    // bottom above the plane and the flat floor breaks into curved bumps
    // taller than the nav step.
    const f32 lift = params.tunnelRadius * 0.45f;
    u32 hopIndex = 0;
    for (const SpaceEdge& edge : graph.edges) {
        for (size_t i = 1; i < edge.path.size(); ++i) {
            const Vec3 a = slotCenter(sp, edge.path[i - 1]);
            const Vec3 b = slotCenter(sp, edge.path[i]);
            const f32 radius = params.tunnelRadius *
                               (edge.kind == EdgeKind::Hidden ? 0.7f : 1.0f);
            // Bend each FLAT hop at a laterally offset midpoint (tree-
            // branch wobble). Ramps stay straight: their two hinged floor
            // planes would crease at the bend into a nav-breaking step —
            // and dug stairs run straight anyway. Shafts stay straight too.
            const Vec3 flat { b.x - a.x, 0.0f, b.z - a.z };
            Vec3 mid = (a + b) * 0.5f;
            if (edge.path[i - 1].floor == edge.path[i].floor &&
                glm::length(flat) > 0.001f) {
                const Vec3 side = glm::normalize(
                    glm::cross(glm::normalize(flat),
                               Vec3 { 0.0f, 1.0f, 0.0f }));
                const f32 t01 =
                    static_cast<f32>(core::hashU32(params.seed ^
                                                   (hopIndex * 2654435761u))) *
                    (1.0f / 4294967295.0f);
                mid += side * params.corridorWobble * (2.0f * t01 - 1.0f);
            }
            ++hopIndex;
            const bool ramp =
                edge.path[i - 1].floor != edge.path[i].floor &&
                glm::length(flat) > 0.001f;
            const Vec3 liftV { 0.0f, lift, 0.0f };
            pipes.push_back({ a + liftV, mid + liftV, radius, a.y,
                              (a.y + b.y) * 0.5f, ramp });
            pipes.push_back({ mid + liftV, b + liftV, radius,
                              (a.y + b.y) * 0.5f, b.y, ramp });
        }
    }

    const f32 margin = params.noiseAmplitude + 2.0f;
    minBounds = Vec3 { 1e9f };
    maxBounds = Vec3 { -1e9f };
    for (const Ball& b : balls) {
        minBounds = glm::min(minBounds, b.center - b.radii - margin);
        maxBounds = glm::max(maxBounds, b.center + b.radii + margin);
    }
    for (const Pipe& p : pipes) {
        const Vec3 r { p.radius + margin };
        minBounds = glm::min(minBounds, glm::min(p.a, p.b) - r);
        maxBounds = glm::max(maxBounds, glm::max(p.a, p.b) + r);
    }
}

f32 DensityField::sample(const Vec3& p) const {
    f32 raw = 1e9f;
    for (const Ball& b : balls) {
        raw = glm::min(raw, sdEllipsoid(p, b.center, b.radii));
    }
    for (const Pipe& pipe : pipes) {
        raw = glm::min(raw, sdCapsule(p, pipe.a, pipe.b, pipe.radius));
    }
    if (raw > params.noiseAmplitude + 1.0f) {
        // Deep rock: skip the noise pass. Sound because raw (no floor cuts)
        // is a lower bound of the final value.
        return raw;
    }
    const f32 n = (render::noise::fbm3(params.seed,
                                       p * (1.0f / params.noiseWavelength),
                                       1.0f, 3, 2.0f, 0.5f) -
                   0.5f) *
                  2.0f * params.noiseAmplitude;
    // Second pass with the noise on walls and the exact floor cut per
    // primitive (see the Ball/Pipe comment in the header).
    f32 d = 1e9f;
    for (const Ball& b : balls) {
        const f32 carve = sdEllipsoid(p, b.center, b.radii) + n;
        d = glm::min(d, glm::max(carve, b.floorY - p.y));
    }
    for (const Pipe& pipe : pipes) {
        const f32 carve = sdCapsule(p, pipe.a, pipe.b, pipe.radius) + n;
        // Floor progress from the HORIZONTAL projection: projecting onto
        // the inclined (lifted) axis skews a ramp's floor plane near its
        // ends. Vertical shafts keep the 3D projection (their floor cut
        // is inert anyway).
        const Vec3 ba = pipe.b - pipe.a;
        const f32 flatLen2 = ba.x * ba.x + ba.z * ba.z;
        const f32 t =
            flatLen2 > 0.001f
                ? glm::clamp(((p.x - pipe.a.x) * ba.x +
                              (p.z - pipe.a.z) * ba.z) /
                                 flatLen2,
                             0.0f, 1.0f)
                : glm::clamp(glm::dot(p - pipe.a, ba) / glm::dot(ba, ba),
                             0.0f, 1.0f);
        const f32 floorAt = pipe.floorA + (pipe.floorB - pipe.floorA) * t;
        d = glm::min(d, glm::max(carve, floorAt - p.y));
    }
    // Ramp under-floor cut (see Pipe::cutsBelow): applied after the union so
    // a neighbouring pipe cannot re-open air under a rising ramp floor.
    // Strictly within the segment's XZ span — clamping t would extend each
    // half-pipe's floor sideways past its ends, cliffing the section below.
    for (const Pipe& pipe : pipes) {
        if (!pipe.cutsBelow) {
            continue;
        }
        const Vec3 ba = pipe.b - pipe.a;
        const f32 flatLen2 = ba.x * ba.x + ba.z * ba.z;
        const f32 t =
            ((p.x - pipe.a.x) * ba.x + (p.z - pipe.a.z) * ba.z) / flatLen2;
        if (t < 0.0f || t > 1.0f) {
            continue;
        }
        const f32 dx = p.x - (pipe.a.x + ba.x * t);
        const f32 dz = p.z - (pipe.a.z + ba.z * t);
        const f32 reach = pipe.radius + params.noiseAmplitude;
        if (dx * dx + dz * dz > reach * reach) {
            continue;
        }
        // A SLAB, not a half-space: the cap pocket sits within `reach`
        // below the floor, and a half-space cut sliced flat walkable
        // shelves into the CEILING of any deeper space the strip crosses
        // (the bandit-in-the-ceiling of the playtests).
        const f32 floorAt = pipe.floorA + (pipe.floorB - pipe.floorA) * t;
        d = glm::max(d, glm::min(floorAt - p.y,
                                 p.y - (floorAt - reach)));
    }
    return d;
}

} // namespace dungeon
