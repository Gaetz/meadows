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
        // Room floors sit at their slot's floor height; the ellipsoid center
        // rises so the carved bottom lands near the floor plane. Tall rooms
        // stretch the vertical axis across their whole floor span.
        const f32 span = static_cast<f32>(room.floorSpan - 1) * sp.floorSpacing;
        const f32 halfHeight = (params.roomHeight + span) * 0.5f;
        const Vec3 base = slotCenter(sp, room.pos);
        ball.center = { base.x, base.y + halfHeight * 0.85f, base.z };
        ball.radii = { room.radius, halfHeight, room.radius };
        ball.floorY = base.y;
        balls.push_back(ball);
    }

    // Corridor centerlines ride one radius above the slot floor so the
    // carved tube's bottom is the walkable plane the nav bake expects.
    const f32 lift = params.tunnelRadius * 0.85f;
    for (const SpaceEdge& edge : graph.edges) {
        for (size_t i = 1; i < edge.path.size(); ++i) {
            Pipe pipe;
            const Vec3 a = slotCenter(sp, edge.path[i - 1]);
            const Vec3 b = slotCenter(sp, edge.path[i]);
            pipe.a = a + Vec3 { 0, lift, 0 };
            pipe.b = b + Vec3 { 0, lift, 0 };
            pipe.radius = params.tunnelRadius *
                          (edge.kind == EdgeKind::Hidden ? 0.7f : 1.0f);
            pipe.floorA = a.y;
            pipe.floorB = b.y;
            pipes.push_back(pipe);
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
        return raw; // deep rock: skip the noise, keep far sampling cheap
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
        const Vec3 ba = pipe.b - pipe.a;
        const f32 t = glm::clamp(glm::dot(p - pipe.a, ba) / glm::dot(ba, ba),
                                 0.0f, 1.0f);
        const f32 floorAt = pipe.floorA + (pipe.floorB - pipe.floorA) * t;
        d = glm::min(d, glm::max(carve, floorAt - p.y));
    }
    return d;
}

Vec3 DensityField::gradient(const Vec3& p) const {
    const f32 e = 0.25f;
    const f32 dx = sample({ p.x + e, p.y, p.z }) - sample({ p.x - e, p.y, p.z });
    const f32 dy = sample({ p.x, p.y + e, p.z }) - sample({ p.x, p.y - e, p.z });
    const f32 dz = sample({ p.x, p.y, p.z + e }) - sample({ p.x, p.y, p.z - e });
    return { dx, dy, dz };
}

} // namespace dungeon
