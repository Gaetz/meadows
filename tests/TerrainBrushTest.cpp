#include <doctest/doctest.h>

#include <cmath>

#include "world/terrain/TerrainBrush.hpp"

using namespace world;

namespace {
constexpr f32 kChunk = 64.0f;
}

TEST_CASE("terrain brush: raise then lower by the same stroke is zero, bit-exact") {
    BrushGrids grids;
    BrushParams brush;
    brush.radius = 6.0f;
    brush.strength = 3.0f;
    const Vec2 center { 30.0f, 30.0f };
    brush.kind = BrushKind::Raise;
    applyTerrainBrush(grids, nullptr, kChunk, brush, center, 0.5f);
    CHECK(brushDeltaAt(grids, kChunk, 30, 30) == doctest::Approx(1.5f));
    CHECK(brushDeltaAt(grids, kChunk, 30, 30) > brushDeltaAt(grids, kChunk, 33, 30));
    CHECK(brushDeltaAt(grids, kChunk, 36, 30) == 0.0f); // at the radius: untouched
    brush.kind = BrushKind::Lower;
    applyTerrainBrush(grids, nullptr, kChunk, brush, center, 0.5f);
    for (const auto& [key, grid] : grids) {
        for (const f32 d : grid.deltas) {
            CHECK(d == 0.0f);
        }
    }
}

TEST_CASE("terrain brush: the falloff is radially symmetric") {
    BrushGrids grids;
    BrushParams brush;
    brush.radius = 8.0f;
    brush.strength = 2.0f;
    applyTerrainBrush(grids, nullptr, kChunk, brush, { 32.0f, 32.0f }, 1.0f);
    const f32 east = brushDeltaAt(grids, kChunk, 35, 32);
    const f32 west = brushDeltaAt(grids, kChunk, 29, 32);
    const f32 north = brushDeltaAt(grids, kChunk, 32, 29);
    const f32 south = brushDeltaAt(grids, kChunk, 32, 35);
    CHECK(east == west);
    CHECK(north == south);
    CHECK(east == north);
    CHECK(east > 0.0f);
    CHECK(east < brushDeltaAt(grids, kChunk, 32, 32));
}

TEST_CASE("terrain brush: a stroke across a chunk edge writes the shared samples alike") {
    BrushGrids grids;
    BrushParams brush;
    brush.radius = 6.0f;
    brush.strength = 2.0f;
    // Centered on the seam x = 64 between chunks 0 and 1.
    applyTerrainBrush(grids, nullptr, kChunk, brush, { 64.0f, 20.0f }, 1.0f);
    REQUIRE(grids.size() == 2);
    const render::HeightPatch& left = grids.at(render::HeightPatches::keyOf(0, 0));
    const render::HeightPatch& right = grids.at(render::HeightPatches::keyOf(1, 0));
    for (u32 row = 0; row < 65; ++row) {
        CHECK(left.deltas[row * 65 + 64] == right.deltas[row * 65 + 0]);
    }
    CHECK(left.deltas[20 * 65 + 64] == doctest::Approx(2.0f));
}

TEST_CASE("terrain brush: publish overrides the published overlay and lists the changed chunks") {
    auto published = std::make_shared<render::HeightPatches>();
    published->chunkSize = kChunk;
    render::HeightPatch authored;
    authored.samples = 65;
    authored.deltas.assign(65 * 65, 1.0f);
    published->chunks[render::HeightPatches::keyOf(5, 5)] = authored;

    BrushGrids grids;
    BrushParams brush;
    brush.radius = 4.0f;
    brush.strength = 1.0f;
    // A stroke inside the authored chunk seeds from it (1 m everywhere).
    applyTerrainBrush(grids, published.get(), kChunk, brush,
                      { 5.0f * kChunk + 32.0f, 5.0f * kChunk + 32.0f }, 1.0f);
    CHECK(brushDeltaAt(grids, kChunk, 5 * 64 + 32, 5 * 64 + 32) ==
          doctest::Approx(2.0f));
    CHECK(brushDeltaAt(grids, kChunk, 5 * 64 + 2, 5 * 64 + 2) == 1.0f);
    vector<u64> changed;
    const auto next = publishBrushGrids(grids, published.get(), kChunk, changed);
    REQUIRE(changed.size() == 1);
    CHECK(changed[0] == render::HeightPatches::keyOf(5, 5));
    CHECK(next->chunks.size() == 1);
    CHECK(next->chunks.at(changed[0]).deltas[32 * 65 + 32] == doctest::Approx(2.0f));
    // The published instance is untouched (immutable for in-flight workers).
    CHECK(published->chunks.at(changed[0]).deltas[32 * 65 + 32] == 1.0f);
}

TEST_CASE("terrain brush: flatten levels toward the target against the live height") {
    BrushGrids grids;
    BrushParams brush;
    brush.kind = BrushKind::Flatten;
    brush.radius = 6.0f;
    brush.flattenTarget = 10.0f;
    // The live ground is a slope: x metres high.
    const LiveHeightFn live = [](f32 x, f32) { return x * 0.5f; };
    // A short application (0.2 s): the pull is gap x min(2.5 x falloff
    // x dt, 1) — half the gap at the center, a quarter of it at half
    // radius (falloff 0.5).
    applyTerrainBrush(grids, nullptr, kChunk, brush, { 30.0f, 30.0f }, 0.2f,
                      live);
    // At the center (live 15 m, target 10): half of the -5 m gap.
    CHECK(brushDeltaAt(grids, kChunk, 30, 30) == doctest::Approx(-2.5f));
    // West of it (live 13.5, gap -3.5): a quarter of it.
    const f32 west = brushDeltaAt(grids, kChunk, 27, 30);
    CHECK(west == doctest::Approx(-0.875f));
    // A long application pulls the full gap — against the LIVE height,
    // which does not see the working delta until it is published (the
    // callers preview between applications, so the gap closes there).
    applyTerrainBrush(grids, nullptr, kChunk, brush, { 30.0f, 30.0f }, 5.0f,
                      live);
    CHECK(brushDeltaAt(grids, kChunk, 30, 30) == doctest::Approx(-7.5f));
    // No live height: flatten is inert.
    BrushGrids none;
    applyTerrainBrush(none, nullptr, kChunk, brush, { 30.0f, 30.0f }, 1.0f);
    CHECK(brushDeltaAt(none, kChunk, 30, 30) == 0.0f);
}

TEST_CASE("terrain brush: a wall rises along its segment, flat on top, rounded at the ends") {
    BrushGrids grids;
    applyTerrainWall(grids, nullptr, kChunk, { 10.0f, 30.0f }, { 40.0f, 30.0f },
                     2.0f, 3.0f);
    // Full height all along the axis, no accumulation between samples.
    for (i32 x = 10; x <= 40; x += 5) {
        CHECK(brushDeltaAt(grids, kChunk, x, 30) == doctest::Approx(3.0f));
    }
    // Symmetric across the axis, zero at the half width.
    CHECK(brushDeltaAt(grids, kChunk, 25, 29) == brushDeltaAt(grids, kChunk, 25, 31));
    CHECK(brushDeltaAt(grids, kChunk, 25, 29) > 0.0f);
    CHECK(brushDeltaAt(grids, kChunk, 25, 29) < 3.0f);
    CHECK(brushDeltaAt(grids, kChunk, 25, 32) == 0.0f);
    // Round caps: a metre past the end still rises, three metres past does not.
    CHECK(brushDeltaAt(grids, kChunk, 41, 30) > 0.0f);
    CHECK(brushDeltaAt(grids, kChunk, 43, 30) == 0.0f);
    // A degenerate segment is a round bump of the half width.
    BrushGrids dot;
    applyTerrainWall(dot, nullptr, kChunk, { 20.0f, 20.0f }, { 20.0f, 20.0f },
                     2.0f, 1.0f);
    CHECK(brushDeltaAt(dot, kChunk, 20, 20) == doctest::Approx(1.0f));
    CHECK(brushDeltaAt(dot, kChunk, 22, 20) == 0.0f);
}
