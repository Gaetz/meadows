#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

// Cubic Bézier / polyline curve toolkit — a MATH utility, not a terrain
// one: rivers smooth their course with it today, roads/splines and any
// future path tool are expected to reuse it (MEADOWS-PLAN P1).

namespace core {

// Cubic Bézier point and tangent at t in [0, 1].
Vec3 bezierPoint(const Vec3& p0, const Vec3& p1, const Vec3& p2,
                 const Vec3& p3, f32 t);
Vec3 bezierTangent(const Vec3& p0, const Vec3& p1, const Vec3& p2,
                   const Vec3& p3, f32 t);

// Ramer-Douglas-Peucker simplification in the XZ plane (y rides along
// with its point): drops collinear/near-collinear points so a smoothing
// pass rounds actual corners instead of chasing grid stair-steps.
vector<Vec3> simplifyPolylineXz(const vector<Vec3>& points, f32 tolerance);

// Smooths an ordered polyline into a piecewise cubic Bézier (Catmull-Rom
// tangents — the curve passes THROUGH the input points) and resamples it
// every ~`spacing` meters. Endpoints are preserved exactly; fewer than 3
// points come back unchanged.
vector<Vec3> smoothPolyline(const vector<Vec3>& points, f32 spacing);

} // namespace core
