#include <doctest/doctest.h>

#include <cmath>

#include "engine/terrain/generation/Authoring.hpp"

// Authoring primitives (v1, headless): stamps, ridgelines, anchors and
// the shape-preserving elevation override the scenario tooling builds on.

using namespace render::terraingen;

namespace {

constexpr u32 kN = 65;
constexpr f32 kTexel = 8.0f;

GridSpec spec() { return GridSpec { 0.0f, 0.0f, kTexel, kN }; }

size_t at(u32 col, u32 row) {
    return static_cast<size_t>(row) * kN + col;
}

} // namespace

TEST_CASE("stampKernel: Add bumps, Max unions, Blend flattens") {
    vector<f32> flat(spec().cells(), 40.0f);

    auto add = flat;
    stampKernel(spec(), add, { 256.0f, 256.0f }, 120.0f, 25.0f,
                StampMode::Add);
    CHECK(add[at(32, 32)] == doctest::Approx(65.0f)); // full core bump
    CHECK(add[at(2, 2)] == 40.0f); // outside: untouched

    auto merged = flat;
    stampKernel(spec(), merged, { 200.0f, 256.0f }, 150.0f, 90.0f,
                StampMode::Max);
    stampKernel(spec(), merged, { 300.0f, 256.0f }, 150.0f, 90.0f,
                StampMode::Max);
    // Between the two peaks both stamps contribute: one merged massif
    // above the plain, peaks at the absolute target.
    CHECK(merged[at(25, 32)] == doctest::Approx(90.0f));
    CHECK(merged[at(31, 32)] > 60.0f);

    auto plateau = flat;
    stampKernel(spec(), plateau, { 256.0f, 256.0f }, 150.0f, 100.0f,
                StampMode::Blend, 0.5f);
    CHECK(plateau[at(32, 32)] == doctest::Approx(100.0f)); // core flat
    CHECK(plateau[at(2, 2)] == 40.0f);
}

TEST_CASE("stampRidge raises a monotone crest out of the ground") {
    vector<f32> h(spec().cells(), 30.0f);
    RidgeStroke stroke;
    stroke.p0 = { 40.0f, 120.0f, 40.0f };
    stroke.p1 = { 180.0f, 130.0f, 200.0f };
    stroke.p2 = { 320.0f, 120.0f, 320.0f };
    stroke.p3 = { 470.0f, 90.0f, 460.0f };
    stroke.crestWidth = 30.0f;
    stroke.falloff = 120.0f;
    stampRidge(spec(), h, stroke);
    // On the crest near the start: close to the control elevation.
    CHECK(h[at(6, 6)] > 100.0f);
    // Far corner: untouched ground.
    CHECK(h[at(62, 2)] == 30.0f);
    // The ridge NEVER digs below the original terrain.
    for (const f32 v : h) {
        CHECK(v >= 30.0f);
    }
}

TEST_CASE("baseElevationAt: exact at anchors, background far away") {
    const ElevationAnchor anchors[] = {
        { 100.0f, 100.0f, 80.0f, 200.0f },
        { 400.0f, 100.0f, 30.0f, 200.0f },
    };
    CHECK(baseElevationAt(anchors, 100.0f, 100.0f, 10.0f) ==
          doctest::Approx(80.0f));
    CHECK(baseElevationAt(anchors, 400.0f, 100.0f, 10.0f) ==
          doctest::Approx(30.0f));
    CHECK(baseElevationAt(anchors, 480.0f, 480.0f, 10.0f) ==
          doctest::Approx(10.0f)); // out of every reach
    // Between the two: a blend, inside their span.
    const f32 mid = baseElevationAt(anchors, 250.0f, 100.0f, 10.0f);
    CHECK(mid > 10.0f);
    CHECK(mid < 80.0f);
}

TEST_CASE("alterElevation levels the center and keeps the shape") {
    // Bumpy ground: a sine field around 50 m.
    vector<f32> h(spec().cells());
    for (u32 row = 0; row < kN; ++row) {
        for (u32 col = 0; col < kN; ++col) {
            h[at(col, row)] =
                50.0f +
                4.0f * std::sin(static_cast<f32>(col) * 0.7f) *
                    std::cos(static_cast<f32>(row) * 0.5f);
        }
    }
    const auto original = h;
    alterElevation(spec(), h, { 256.0f, 256.0f }, 160.0f, 90.0f);
    // The center reached the target...
    CHECK(h[at(32, 32)] == doctest::Approx(90.0f));
    // ...neighbours moved by (almost) the same delta: the bumps SHIFTED
    // instead of flattening — local relief is preserved.
    const f32 deltaCenter = h[at(32, 32)] - original[at(32, 32)];
    const f32 deltaNear = h[at(34, 32)] - original[at(34, 32)];
    CHECK(std::abs(deltaNear - deltaCenter) < deltaCenter * 0.1f);
    // Outside the radius: untouched.
    CHECK(h[at(2, 2)] == original[at(2, 2)]);
}
