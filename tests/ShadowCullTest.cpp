#include <doctest/doctest.h>

#include "engine/render/Frustum.hpp"
#include "engine/render/landscape/ShadowMapper.hpp"

// The shadow-caster cull contract: casters are tested against each
// cascade's LIGHT-SPACE ortho volume (Frustum::fromViewProj of the
// cascade matrix). The fitted ortho already extends kCasterReach behind
// the slice toward the sun, so an off-screen tower whose shadow falls
// into the slice must SURVIVE the cull — that reach is what these cases
// pin. computeCascades is pure math; no GPU involved.

namespace {

render::Frustum cascadeFrustum(u32 i, const Vec3& sunDirection) {
    render::Camera3D camera; // origin, yaw 0 -> looking down -Z
    const render::ShadowMapper mapper;
    const render::ShadowMapper::Cascades cascades =
        mapper.computeCascades(camera, 16.0f / 9.0f, sunDirection);
    return render::Frustum::fromViewProj(cascades.viewProj[i]);
}

bool boxVisible(const render::Frustum& frustum, const Vec3& center,
                f32 half = 1.0f) {
    return frustum.intersectsAabb(center - Vec3 { half },
                                  center + Vec3 { half });
}

} // namespace

TEST_CASE("shadow cull: casters toward the sun survive the cascade cull") {
    const Vec3 sun = glm::normalize(Vec3 { 0.4f, 0.8f, 0.2f });
    const render::Frustum cascade0 = cascadeFrustum(0, sun);
    const Vec3 sliceCenter { 0.0f, 0.0f, -45.0f }; // mid cascade-0 slice

    // A prop inside the slice.
    CHECK(boxVisible(cascade0, { 0.0f, 0.0f, -30.0f }));
    // The off-screen tower: outside the camera slice but along the sun
    // ray — its shadow lands in the slice (kCasterReach covers 350 m).
    CHECK(boxVisible(cascade0, sliceCenter + sun * 300.0f));
    // Beyond the reach, behind the light camera: culled.
    CHECK_FALSE(boxVisible(cascade0, sliceCenter + sun * 800.0f));
    // Far to the side of the ortho extent: culled.
    CHECK_FALSE(boxVisible(cascade0, { 2000.0f, 0.0f, -45.0f }));
    // Far past the light far plane (opposite the sun): culled.
    CHECK_FALSE(boxVisible(cascade0, sliceCenter - sun * 800.0f));
}
