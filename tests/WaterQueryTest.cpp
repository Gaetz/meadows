#include <doctest/doctest.h>

#include "engine/terrain/WaterQuery.hpp"

// R3 break-case suite (docs/WATER-RENDER.md): the unified gameplay
// water sample must (1) let the SIM answer inside its trusted rect —
// water AND dryness, (2) fall back to the baked bodies outside or
// without a snapshot, (3) keep the sea swimmable in the authority
// zone (sim publishes sea cells dry), (4) read DRY in a gallery under
// the water column, (5) hand the sim current to the swim drift.

using namespace render::terrain;

namespace {

// A 65x65 @ 2 m snapshot (margin 8 cells) with a wet block at a
// chosen surface/depth/current.
WaterSimSnapshot makeSnap(f32 surface, f32 depth, f32 velX) {
    WaterSimSnapshot snap;
    snap.spec.originX = 0.0f;
    snap.spec.originZ = 0.0f;
    snap.spec.texelSize = 2.0f;
    snap.spec.n = 65;
    snap.marginCells = 8;
    const size_t cells = snap.spec.cells();
    snap.surface.assign(cells, kWaterInfoDry);
    snap.depth.assign(cells, 0.0f);
    snap.velX.assign(cells, 0.0f);
    snap.velZ.assign(cells, 0.0f);
    for (u32 r = 25; r <= 40; ++r) {
        for (u32 c = 25; c <= 40; ++c) {
            const size_t i = static_cast<size_t>(r) * snap.spec.n + c;
            snap.surface[i] = surface;
            snap.depth[i] = depth;
            snap.velX[i] = velX;
        }
    }
    return snap;
}

render::WaterBodies makeBodies(f32 lakeLevel) {
    render::WaterBodies bodies;
    bodies.seaLevel = 0.0f;
    render::LakeSurface lake;
    lake.level = lakeLevel;
    lake.minX = 0.0f;
    lake.minZ = 0.0f;
    lake.maxX = 110.0f;
    lake.maxZ = 110.0f; // maskless: bbox footprint
    bodies.lakes.push_back(lake);
    return bodies;
}

} // namespace

TEST_CASE("water query: the sim answers inside its rect, water AND dryness") {
    const WaterSimSnapshot snap = makeSnap(203.0f, 2.0f, 1.5f);
    const render::WaterBodies bodies = makeBodies(210.0f);
    WaterQuery q { &snap, &bodies, 0.0f };
    // Wet sim cell (block center ~ (65, 65)): the SIM surface wins
    // over the baked lake claim (210).
    const auto s = waterSurfaceQuery(q, 65.0f, 65.0f, 203.5f);
    REQUIRE(s.has_value());
    CHECK(*s == doctest::Approx(203.0f).epsilon(0.001));
    // Dry sim cell inside the rect, under the baked lake bbox: DRY —
    // the underground-blue fix (baked claims yield to sim dryness).
    CHECK(!waterSurfaceQuery(q, 100.0f, 100.0f, 205.0f).has_value());
    // The current is the sim's, not the baked stillness.
    const Vec2 flow = waterFlowQuery(q, 65.0f, 65.0f, 203.5f);
    CHECK(flow.x == doctest::Approx(1.5f).epsilon(0.01));
}

TEST_CASE("water query: baked fallback outside the rect and without sim") {
    const render::WaterBodies bodies = makeBodies(210.0f);
    // No snapshot at all (pre-roll, settling): baked answers.
    WaterQuery noSim { nullptr, &bodies, 0.0f };
    const auto s = waterSurfaceQuery(noSim, 65.0f, 65.0f, 209.0f);
    REQUIRE(s.has_value());
    CHECK(*s == doctest::Approx(210.0f).epsilon(0.001));
    // Snapshot present but the probe is OUTSIDE the trusted rect
    // (margin 8 + 2 texels = 20 m inset; x=10 is in the margin).
    const WaterSimSnapshot snap = makeSnap(203.0f, 2.0f, 0.0f);
    WaterQuery q { &snap, &bodies, 0.0f };
    const auto edge = waterSurfaceQuery(q, 10.0f, 65.0f, 209.0f);
    REQUIRE(edge.has_value());
    CHECK(*edge == doctest::Approx(210.0f).epsilon(0.001));
}

TEST_CASE("water query: the sea stays swimmable in the authority zone") {
    const WaterSimSnapshot snap = makeSnap(203.0f, 2.0f, 0.0f);
    render::WaterBodies bodies;
    bodies.seaLevel = 0.0f;
    WaterQuery q { &snap, &bodies, 0.0f };
    // Dry sim cell (sea publishes dry by doctrine), probe at sea
    // level: the analytic sheet answers.
    const auto sea = waterSurfaceQuery(q, 100.0f, 100.0f, -0.5f);
    REQUIRE(sea.has_value());
    CHECK(*sea == doctest::Approx(0.0f).epsilon(0.001));
    // Probe well above the sea: dry.
    CHECK(!waterSurfaceQuery(q, 100.0f, 100.0f, 40.0f).has_value());
}

TEST_CASE("water query: a gallery under the column reads dry") {
    const WaterSimSnapshot snap = makeSnap(203.0f, 2.0f, 0.0f);
    WaterQuery q { &snap, nullptr, -1.0e6f };
    // In the column: wet.
    CHECK(waterSurfaceQuery(q, 65.0f, 65.0f, 202.0f).has_value());
    // 10 m below its bottom (201): a cave under the river — dry.
    CHECK(!waterSurfaceQuery(q, 65.0f, 65.0f, 191.0f).has_value());
    // Far above the surface: dry too (nobody swims in the sky).
    CHECK(!waterSurfaceQuery(q, 65.0f, 65.0f, 215.0f).has_value());
}
