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
