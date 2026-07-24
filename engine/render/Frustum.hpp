#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

namespace render {

// View frustum as six inward-facing planes extracted from a viewProj matrix
// (Gribb-Hartmann). Pure math, headless-testable — the CPU chunk-culling
// primitive. Works for any projection the matrix encodes; note
// that Lengyel's oblique near-plane trick corrupts the FAR plane, so build
// reflection-pass frusta from the NON-oblique projection (conservative).
struct Frustum {
    array<Vec4, 6> planes {}; // xyz = normal, w: inside <=> n·p + w >= 0

    static Frustum fromViewProj(const Mat4& viewProj) {
        Frustum frustum;
        // glm is column-major: row i of the matrix is (m[0][i] .. m[3][i]).
        const auto row = [&](int i) {
            return Vec4 { viewProj[0][i], viewProj[1][i], viewProj[2][i],
                          viewProj[3][i] };
        };
        const Vec4 r0 = row(0);
        const Vec4 r1 = row(1);
        const Vec4 r2 = row(2);
        const Vec4 r3 = row(3);
        frustum.planes[0] = r3 + r0; // left
        frustum.planes[1] = r3 - r0; // right
        frustum.planes[2] = r3 + r1; // bottom
        frustum.planes[3] = r3 - r1; // top
        frustum.planes[4] = r2;      // near (0..1 clip: 0 <= z)
        frustum.planes[5] = r3 - r2; // far
        for (Vec4& plane : frustum.planes) {
            const f32 length = glm::length(Vec3 { plane });
            if (length > 1e-8f) {
                plane /= length;
            }
        }
        return frustum;
    }

    // Conservative sphere test (same contract as the AABB test below):
    // false only when the sphere is fully outside one plane.
    bool intersectsSphere(const Vec3& center, f32 radius) const {
        for (const Vec4& plane : planes) {
            if (glm::dot(Vec3 { plane }, center) + plane.w < -radius) {
                return false;
            }
        }
        return true;
    }

    // Conservative AABB test: rejects only when the box is fully outside
    // one plane (the classic false-positive corner cases keep drawing —
    // harmless for culling).
    bool intersectsAabb(const Vec3& lo, const Vec3& hi) const {
        for (const Vec4& plane : planes) {
            const Vec3 farCorner { plane.x >= 0.0f ? hi.x : lo.x,
                                   plane.y >= 0.0f ? hi.y : lo.y,
                                   plane.z >= 0.0f ? hi.z : lo.z };
            if (glm::dot(Vec3 { plane }, farCorner) + plane.w < 0.0f) {
                return false;
            }
        }
        return true;
    }
};

} // namespace render
