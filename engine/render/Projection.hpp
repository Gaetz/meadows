#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

namespace render {

// Lengyel's oblique near plane, 0..1-clip variant (GLM_FORCE_DEPTH_ZERO_TO_ONE
// is global — depth 0..1, docs/VULKAN.md): bends the projection's near plane
// onto an arbitrary view-space plane, so a mirrored render (water reflection)
// clips everything below the surface for free — no user clip distance in the
// shaders. `clipPlaneView` is the plane in VIEW space (xyz normal, w), with
// the kept half-space on the positive side.
//
// Derivation: under 0..1 clip the near plane is z_clip = 0, so the new z row
// must be PROPORTIONAL to the plane itself (dot = 0 exactly on it) — simpler
// than the -1..1 variant, which needed the `c - row3` / `+1` dance. The scale
// pins z_ndc = 1 at q, the far corner most opposite the plane, keeping the
// depth range as tight as the trick allows. Header-only and pure math so the
// headless suite proves it (ProjectionTest) — this cannot be eyeballed.
// Lengyel's caveat stands: the FAR plane of the result is corrupted, so cull
// with the non-oblique projection (see Frustum.hpp).
inline Mat4 obliqueProjection(Mat4 proj, const Vec4& clipPlaneView) {
    Vec4 q;
    q.x = (glm::sign(clipPlaneView.x) + proj[2][0]) / proj[0][0];
    q.y = (glm::sign(clipPlaneView.y) + proj[2][1]) / proj[1][1];
    q.z = -1.0f;
    q.w = (1.0f + proj[2][2]) / proj[3][2];
    const Vec4 c = clipPlaneView / glm::dot(clipPlaneView, q);
    proj[0][2] = c.x;
    proj[1][2] = c.y;
    proj[2][2] = c.z;
    proj[3][2] = c.w;
    return proj;
}

} // namespace render
