#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

namespace render {

// Lengyel's oblique near plane, REVERSED-Z 0..1-clip variant
// (GLM_FORCE_DEPTH_ZERO_TO_ONE is global; the camera path renders
// reversed — near at ndc 1, far at ndc 0, docs/RENDERING.md §6.0a):
// bends the projection's near boundary onto an arbitrary view-space
// plane, so a mirrored render (water reflection) clips everything below
// the surface for free — no user clip distance in the shaders.
// `clipPlaneView` is the plane in VIEW space (xyz normal, w), with the
// kept half-space on the positive side.
//
// Derivation: reversed-Z clips z_clip outside [0, w] with NEAR at
// z_clip = w. The new z row pins the plane exactly there
// (rowZ' = rowW − C/dot(C,q): on the plane dot(C,p)=0 → z=w; the
// discarded side lands z>w) and scales so z_clip = 0 at q, the corner
// of the reversed FAR boundary (ndc z = 0) most opposite the plane —
// (P·q).w = 1 by construction, which is what makes rowW−c land 0
// there. Header-only and pure math so the headless suite proves it
// (ProjectionTest) — this cannot be eyeballed. Lengyel's caveat
// stands: the FAR plane of the result is corrupted, so cull with the
// non-oblique projection (see Frustum.hpp).
inline Mat4 obliqueProjection(Mat4 proj, const Vec4& clipPlaneView) {
    const Vec4 q = glm::inverse(proj) *
                   Vec4 { glm::sign(clipPlaneView.x),
                          glm::sign(clipPlaneView.y), 0.0f, 1.0f };
    const Vec4 c = clipPlaneView / glm::dot(clipPlaneView, q);
    proj[0][2] = proj[0][3] - c.x;
    proj[1][2] = proj[1][3] - c.y;
    proj[2][2] = proj[2][3] - c.z;
    proj[3][2] = proj[3][3] - c.w;
    return proj;
}

} // namespace render
