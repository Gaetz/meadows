#include "engine/core/Bezier.hpp"

#include <cmath>

namespace core {

Vec3 bezierPoint(const Vec3& p0, const Vec3& p1, const Vec3& p2,
                 const Vec3& p3, f32 t) {
    const f32 u = 1.0f - t;
    return u * u * u * p0 + 3.0f * u * u * t * p1 + 3.0f * u * t * t * p2 +
           t * t * t * p3;
}

Vec3 bezierTangent(const Vec3& p0, const Vec3& p1, const Vec3& p2,
                   const Vec3& p3, f32 t) {
    const f32 u = 1.0f - t;
    return 3.0f * u * u * (p1 - p0) + 6.0f * u * t * (p2 - p1) +
           3.0f * t * t * (p3 - p2);
}

namespace {

f32 distToChordXz(const Vec3& p, const Vec3& a, const Vec3& b) {
    const f32 abx = b.x - a.x;
    const f32 abz = b.z - a.z;
    const f32 len2 = abx * abx + abz * abz;
    const f32 t =
        len2 > 0.0f
            ? glm::clamp(((p.x - a.x) * abx + (p.z - a.z) * abz) / len2,
                         0.0f, 1.0f)
            : 0.0f;
    const f32 dx = p.x - (a.x + abx * t);
    const f32 dz = p.z - (a.z + abz * t);
    return std::sqrt(dx * dx + dz * dz);
}

void rdp(const vector<Vec3>& points, size_t first, size_t last,
         f32 tolerance, vector<u8>& keep) {
    if (last <= first + 1) {
        return;
    }
    f32 worst = -1.0f;
    size_t worstAt = first;
    for (size_t i = first + 1; i < last; ++i) {
        const f32 d =
            distToChordXz(points[i], points[first], points[last]);
        if (d > worst) {
            worst = d;
            worstAt = i;
        }
    }
    if (worst > tolerance) {
        keep[worstAt] = 1;
        rdp(points, first, worstAt, tolerance, keep);
        rdp(points, worstAt, last, tolerance, keep);
    }
}

} // namespace

vector<Vec3> simplifyPolylineXz(const vector<Vec3>& points,
                                f32 tolerance) {
    if (points.size() < 3) {
        return points;
    }
    vector<u8> keep(points.size(), 0);
    keep.front() = 1;
    keep.back() = 1;
    rdp(points, 0, points.size() - 1, tolerance, keep);
    vector<Vec3> out;
    out.reserve(points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        if (keep[i]) {
            out.push_back(points[i]);
        }
    }
    return out;
}

vector<Vec3> smoothPolyline(const vector<Vec3>& points, f32 spacing) {
    if (points.size() < 3 || spacing <= 0.0f) {
        return points;
    }
    const auto tangentAt = [&](size_t i) {
        const Vec3& prev = points[i > 0 ? i - 1 : 0];
        const Vec3& next = points[glm::min(i + 1, points.size() - 1)];
        return (next - prev) * 0.5f;
    };
    vector<Vec3> out;
    out.reserve(points.size() * 3);
    out.push_back(points.front());
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        const Vec3& a = points[i];
        const Vec3& b = points[i + 1];
        // Catmull-Rom tangents as Bézier control points: interpolating,
        // C1 across joints.
        const Vec3 c1 = a + tangentAt(i) / 3.0f;
        const Vec3 c2 = b - tangentAt(i + 1) / 3.0f;
        const f32 length = glm::distance(a, b);
        const u32 steps = glm::max(
            1u, static_cast<u32>(std::ceil(length / spacing)));
        for (u32 s = 1; s <= steps; ++s) {
            const f32 t = static_cast<f32>(s) / static_cast<f32>(steps);
            out.push_back(bezierPoint(a, c1, c2, b, t));
        }
    }
    return out;
}

} // namespace core
