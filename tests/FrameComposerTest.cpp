#include <doctest/doctest.h>

#include "engine/render/FrameComposer.hpp"

// The per-frame UBO composition — extracted from
// LandscapeScene::render() precisely so these invariants get locked
// headless: the interior reshape, the A/B-neutral toggles, the reflection
// contract (base stays unpatched) and the .w-slot appends (the UBO lesson:
// each free .w carries a specific feature — a regression here corrupts a
// shader without any compile error).

namespace {

render::FrameComposerInputs exteriorDay() {
    render::FrameComposerInputs in;
    in.viewProj = Mat4 { 1.0f };
    in.cameraPosition = { 10.0f, 50.0f, -20.0f };
    in.width = 1920;
    in.height = 1080;
    in.dt = 0.016f;
    in.timeSeconds = 42.0f;
    in.sky.sunDirection = { 0.0f, 1.0f, 0.0f }; // noon
    in.sky.sunColor = { 1.0f, 0.95f, 0.9f };
    in.sky.ambientColor = { 0.4f, 0.45f, 0.5f };
    in.atmos.fogDensity = 0.0014f;
    in.atmos.rainIntensity = 0.0f;
    in.seaLevel = 8.0f;
    in.snowLine = 110.0f;
    in.interiorAmbient = { 0.16f, 0.15f, 0.14f };
    return in;
}

} // namespace

TEST_CASE("frame composer: exterior passes sky and toggles through") {
    render::FrameComposerInputs in = exteriorDay();
    in.stylized = true;
    in.tonemap = true;
    in.exposure = 1.3f;
    const auto out = render::composeFrameUniforms(in);

    CHECK(out.resolved.sunColor.x == doctest::Approx(1.0f));
    CHECK(out.resolved.ambientColor.w == doctest::Approx(1.0f)); // stylized
    CHECK(out.resolved.postInfo.x == doctest::Approx(1.0f));     // tonemap
    CHECK(out.resolved.postInfo.y == doctest::Approx(1.3f));     // exposure
    CHECK(out.resolved.cascadeSplits.w == doctest::Approx(0.0f)); // exterior
    CHECK(out.resolved.fogInfo.x == doctest::Approx(0.0014f));
    CHECK(out.resolved.screenInfo.x == doctest::Approx(1920.0f));
    CHECK(out.resolved.screenInfo.z == doctest::Approx(1.0f / 1920.0f));
}

TEST_CASE("frame composer: interior mode reshapes the frame") {
    render::FrameComposerInputs in = exteriorDay();
    in.interiorMode = true;
    in.atmos.volumetric = 1.0f;
    in.atmos.rainIntensity = 0.8f; // must be gated off indoors
    const auto out = render::composeFrameUniforms(in);

    CHECK(out.resolved.sunColor.x == doctest::Approx(0.0f));
    CHECK(out.resolved.sunColor.w == doctest::Approx(0.0f)); // no disc
    CHECK(out.resolved.ambientColor.x == doctest::Approx(0.16f));
    CHECK(out.resolved.fogInfo.x == doctest::Approx(0.0f)); // no fog
    CHECK(out.resolved.time.z == doctest::Approx(0.0f));    // volumetric off
    CHECK(out.resolved.cascadeSplits.w == doctest::Approx(1.0f));
    CHECK(out.resolved.stormInfo.y == doctest::Approx(0.0f)); // no rain
    // The reflection contract: `base` keeps the raw exterior composition.
    CHECK(out.base.sunColor.x == doctest::Approx(1.0f));
    CHECK(out.base.fogInfo.x == doctest::Approx(0.0014f));
}

TEST_CASE("frame composer: grading toggle is neutral when off") {
    render::FrameComposerInputs in = exteriorDay();
    in.grading = false;
    in.gradeVibrance = 0.3f;
    in.gradeSplitTone = 0.35f;
    in.gradeContrast = 1.06f;
    auto out = render::composeFrameUniforms(in);
    CHECK(out.resolved.sunGlowColor.w == doctest::Approx(0.0f));
    CHECK(out.resolved.zenithColor.w == doctest::Approx(0.0f));
    CHECK(out.resolved.horizonColor.w == doctest::Approx(1.0f)); // neutral

    in.grading = true;
    out = render::composeFrameUniforms(in);
    CHECK(out.resolved.sunGlowColor.w == doctest::Approx(0.3f));
    CHECK(out.resolved.zenithColor.w == doctest::Approx(0.35f));
    CHECK(out.resolved.horizonColor.w == doctest::Approx(1.06f));
}

TEST_CASE("frame composer: auto-exposure rides the free .w slots") {
    render::FrameComposerInputs in = exteriorDay();
    in.autoExposure = true;
    in.autoExposureMin = 0.4f;
    in.autoExposureMax = 2.5f;
    const auto out = render::composeFrameUniforms(in);
    CHECK(out.resolved.sunDirection.w == doctest::Approx(0.016f)); // dt
    CHECK(out.resolved.horizonFarColor.w == doctest::Approx(0.4f));
    CHECK(out.resolved.cloudMapInfo.w == doctest::Approx(2.5f));
    CHECK(out.resolved.windInfo.w == doctest::Approx(1.0f));
}

TEST_CASE("frame composer: sun on screen drives the god-ray fade") {
    render::FrameComposerInputs in = exteriorDay();
    // Identity viewProj; the sun points straight up — off screen.
    in.sky.sunDirection = { 0.0f, 0.1f, 0.0f };
    in.viewProj = Mat4 { 1.0f };
    in.cameraPosition = { 0.0f, 0.0f, 0.0f };
    const auto out = render::composeFrameUniforms(in);
    // clip = (0, 100, 0, 1) -> ndc.y = 100 -> off screen: fade 0, uv kept.
    CHECK(out.resolved.sunScreen.z == doctest::Approx(0.0f));

    render::FrameComposerInputs centered = exteriorDay();
    centered.cameraPosition = { 0.0f, 0.0f, 0.0f };
    centered.sky.sunDirection = { 0.0f, 0.0f, 0.0f }; // degenerate: uv center
    const auto out2 = render::composeFrameUniforms(centered);
    CHECK(out2.resolved.sunScreen.x == doctest::Approx(0.5f));
    CHECK(out2.resolved.sunScreen.y == doctest::Approx(0.5f));
}

TEST_CASE("frame composer: rain builds the occlusion matrix, dry does not") {
    render::FrameComposerInputs in = exteriorDay();
    in.atmos.rainIntensity = 0.5f;
    in.atmos.stormFront = 0.7f;
    const auto out = render::composeFrameUniforms(in);
    CHECK(out.resolved.stormInfo.x == doctest::Approx(0.7f));
    CHECK(out.resolved.stormInfo.y == doctest::Approx(0.5f));
    // The ortho matrix is non-identity when raining.
    CHECK(out.resolved.rainOcclusionViewProj != Mat4 { 1.0f });

    render::FrameComposerInputs dry = exteriorDay();
    const auto outDry = render::composeFrameUniforms(dry);
    CHECK(outDry.resolved.rainOcclusionViewProj == Mat4 {});
}

TEST_CASE("frame composer: grass bend follows the player's feet in Play") {
    render::FrameComposerInputs in = exteriorDay();
    in.grassBend = true;
    in.playerFeet = { 3.0f, 12.0f, -7.0f };
    const auto out = render::composeFrameUniforms(in);
    CHECK(out.resolved.grassBendInfo.x == doctest::Approx(3.0f));
    CHECK(out.resolved.grassBendInfo.y == doctest::Approx(-7.0f)); // XZ first
    CHECK(out.resolved.grassBendInfo.z == doctest::Approx(12.0f));
    CHECK(out.resolved.grassBendInfo.w == doctest::Approx(0.85f));

    in.grassBend = false;
    const auto off = render::composeFrameUniforms(in);
    CHECK(off.resolved.grassBendInfo.w == doctest::Approx(0.0f));
}

TEST_CASE("frame composer: stormInfo.x carries the raw storm front") {
    // (.x is the plain crossfaded front; rain/wetness/occlusion ride .y.)
    render::FrameComposerInputs in = exteriorDay();
    in.atmos.stormFront = 0.35f;
    in.atmos.cloudCoverage = 0.6f; // coverage no longer feeds .x
    CHECK(render::composeFrameUniforms(in).resolved.stormInfo.x ==
          doctest::Approx(0.35f));
}
