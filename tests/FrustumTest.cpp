#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/render/Frustum.hpp"

using render::Frustum;

namespace {

Frustum makeFrustum() {
    // Camera at origin looking down -Z, 60° fov, near 0.1, far 100.
    const Mat4 proj =
        glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    const Mat4 view = glm::lookAt(Vec3 { 0.0f }, Vec3 { 0.0f, 0.0f, -1.0f },
                                  Vec3 { 0.0f, 1.0f, 0.0f });
    return Frustum::fromViewProj(proj * view);
}

} // namespace

TEST_CASE("AABB in front of the camera intersects the frustum") {
    const Frustum frustum = makeFrustum();
    CHECK(frustum.intersectsAabb({ -1.0f, -1.0f, -11.0f },
                                 { 1.0f, 1.0f, -9.0f }));
}

TEST_CASE("AABBs outside each side are rejected") {
    const Frustum frustum = makeFrustum();
    // Behind the camera.
    CHECK_FALSE(frustum.intersectsAabb({ -1.0f, -1.0f, 9.0f },
                                       { 1.0f, 1.0f, 11.0f }));
    // Beyond the far plane.
    CHECK_FALSE(frustum.intersectsAabb({ -1.0f, -1.0f, -220.0f },
                                       { 1.0f, 1.0f, -180.0f }));
    // Far off to the left/right at a depth where the frustum is narrow.
    CHECK_FALSE(frustum.intersectsAabb({ -80.0f, -1.0f, -11.0f },
                                       { -60.0f, 1.0f, -9.0f }));
    CHECK_FALSE(frustum.intersectsAabb({ 60.0f, -1.0f, -11.0f },
                                       { 80.0f, 1.0f, -9.0f }));
    // High above / far below.
    CHECK_FALSE(frustum.intersectsAabb({ -1.0f, 60.0f, -11.0f },
                                       { 1.0f, 80.0f, -9.0f }));
    CHECK_FALSE(frustum.intersectsAabb({ -1.0f, -80.0f, -11.0f },
                                       { 1.0f, -60.0f, -9.0f }));
}

TEST_CASE("AABB straddling a frustum edge is kept (conservative)") {
    const Frustum frustum = makeFrustum();
    // Huge box surrounding the whole frustum.
    CHECK(frustum.intersectsAabb({ -500.0f, -500.0f, -500.0f },
                                 { 500.0f, 500.0f, 500.0f }));
    // Box crossing the near plane.
    CHECK(frustum.intersectsAabb({ -1.0f, -1.0f, -1.0f },
                                 { 1.0f, 1.0f, 1.0f }));
}

TEST_CASE("terrain-chunk-like AABBs cull left and right of the view") {
    // Camera 100 m up, looking along +X (typical fly-cam pose).
    const Mat4 proj =
        glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 2000.0f);
    const Mat4 view =
        glm::lookAt(Vec3 { 0.0f, 100.0f, 0.0f }, Vec3 { 100.0f, 80.0f, 0.0f },
                    Vec3 { 0.0f, 1.0f, 0.0f });
    const Frustum frustum = Frustum::fromViewProj(proj * view);

    // Chunk ahead: visible. Chunk straight behind: culled.
    CHECK(frustum.intersectsAabb({ 320.0f, 0.0f, -32.0f },
                                 { 384.0f, 60.0f, 32.0f }));
    CHECK_FALSE(frustum.intersectsAabb({ -384.0f, 0.0f, -32.0f },
                                       { -320.0f, 60.0f, 32.0f }));
}

// --- Depth 0..1 (the Vulkan convention) -----------------------------------------
// GLM_FORCE_DEPTH_ZERO_TO_ONE is global; these tests pin the convention so a
// build without the define (or a frustum/oblique regression) fails headless —
// none of this is verifiable by eye on the M1.

#include "engine/render/Projection.hpp"

TEST_CASE("depth 0..1: glm projects near to 0, far to 1") {
    const Mat4 proj =
        glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    const Vec4 nearClip = proj * Vec4 { 0.0f, 0.0f, -0.1f, 1.0f };
    CHECK(nearClip.z / nearClip.w == doctest::Approx(0.0f).epsilon(1e-4));
    const Vec4 farClip = proj * Vec4 { 0.0f, 0.0f, -100.0f, 1.0f };
    CHECK(farClip.z / farClip.w == doctest::Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("reversed-Z: swapped near/far args project near to 1, far to 0") {
    // The Camera3D::proj idiom (docs/RENDERING.md §6.0a) — pinned here so
    // a glm behavior change or an accidental un-swap fails headless.
    const Mat4 proj =
        glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 100.0f, 0.1f);
    const Vec4 nearClip = proj * Vec4 { 0.0f, 0.0f, -0.1f, 1.0f };
    CHECK(nearClip.z / nearClip.w == doctest::Approx(1.0f).epsilon(1e-4));
    const Vec4 farClip = proj * Vec4 { 0.0f, 0.0f, -100.0f, 1.0f };
    CHECK(farClip.z / farClip.w ==
          doctest::Approx(0.0f).epsilon(1e-4));
}

TEST_CASE("depth 0..1: frustum near plane culls behind the eye") {
    const Frustum frustum = makeFrustum();
    // In front of the near plane: inside. Behind the eye: outside — under
    // -1..1 extraction (r3 + r2) this second box would NOT be culled.
    CHECK(frustum.intersectsAabb({ -1.0f, -1.0f, -5.0f },
                                 { 1.0f, 1.0f, -4.0f }));
    CHECK_FALSE(frustum.intersectsAabb({ -1.0f, -1.0f, 4.0f },
                                       { 1.0f, 1.0f, 5.0f }));
    CHECK_FALSE(frustum.intersectsAabb({ -1.0f, -1.0f, 0.02f },
                                       { 1.0f, 1.0f, 0.05f }));
}

TEST_CASE("reversed-Z oblique near plane (water reflection)") {
    // The camera path is reversed, so obliqueProjection only ever
    // receives reversed projections (near ndc 1, far ndc 0).
    const Mat4 proj =
        glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 100.0f, 0.1f);
    // View-space plane y = -2, keeping everything above (y + 2 >= 0) — the
    // shape of the water plane seen from the mirrored camera.
    const Vec4 plane { 0.0f, 1.0f, 0.0f, 2.0f };
    const Mat4 oblique = render::obliqueProjection(proj, plane);

    const auto ndcZ = [&](const Vec3& p) {
        const Vec4 clip = oblique * Vec4 { p, 1.0f };
        return clip.z / clip.w;
    };
    // Exactly on the plane -> the new near (reversed: z_ndc = 1),
    // wherever along it.
    CHECK(ndcZ({ 0.0f, -2.0f, -10.0f }) == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(ndcZ({ 3.0f, -2.0f, -40.0f }) == doctest::Approx(1.0f).epsilon(1e-4));
    // Above the plane: kept (~0 <= z < 1); below: clipped (z > 1 = past
    // the reversed near boundary).
    const f32 above = ndcZ({ 0.0f, 0.0f, -10.0f });
    CHECK(above < 1.0f);
    CHECK(above >= -1e-3f);
    CHECK(ndcZ({ 0.0f, -3.0f, -10.0f }) > 1.0f);
    // xy are untouched: only the z row changed.
    const Vec4 a = proj * Vec4 { 1.0f, 2.0f, -10.0f, 1.0f };
    const Vec4 b = oblique * Vec4 { 1.0f, 2.0f, -10.0f, 1.0f };
    CHECK(a.x == doctest::Approx(b.x));
    CHECK(a.y == doctest::Approx(b.y));
    CHECK(a.w == doctest::Approx(b.w));
}
