#include <doctest/doctest.h>

#include <cmath>

#include "engine/terrain/generation/WaterSolve.hpp"

// Option-D water solve (docs/WATER-RESEARCH.md): rain on a tilted
// valley must gather into a flowing channel that drains to the low
// edge, the sea stays pinned, and the whole thing is bit-deterministic.

using namespace render::terraingen;

namespace {

constexpr u32 kN = 65;
constexpr f32 kTexel = 8.0f;

GridSpec spec() { return GridSpec { 0.0f, 0.0f, kTexel, kN }; }

size_t at(u32 col, u32 row) {
    return static_cast<size_t>(row) * kN + col;
}

} // namespace

TEST_CASE("water solve: rain gathers into a draining channel") {
    // V valley tilted toward +z, ending in sea cells on the last rows.
    vector<f32> ground(spec().cells());
    const f32 axis = static_cast<f32>(kN / 2);
    for (u32 row = 0; row < kN; ++row) {
        for (u32 col = 0; col < kN; ++col) {
            const f32 lateral =
                std::abs(static_cast<f32>(col) - axis) * 1.2f;
            const f32 down = static_cast<f32>(kN - 1 - row) * 0.5f;
            ground[at(col, row)] = 24.0f + lateral + down;
        }
    }
    for (u32 row = kN - 4; row < kN; ++row) {
        for (u32 col = 0; col < kN; ++col) {
            ground[at(col, row)] = 10.0f; // sea floor (under seaLevel 21)
        }
    }
    WaterSolveParams params;
    params.maxIterations = 8000;
    // Storm-grade rain: the tiny grid's catchments must build a
    // visible channel within the iteration budget (the game-scale
    // default spreads a centimeter film across the 3-cell channel and
    // the dry threshold eats it).
    params.rainRate = 3.0e-4f;
    params.evaporationRate = 2.0e-5f;
    params.dryThreshold = 0.01f;
    const WaterSolveResult water =
        solveSteadyWater(spec(), ground, params);
    CHECK(water.iterations > 0);

    // The channel: mid-valley cells carry water flowing toward +z;
    // the ridges stay dry.
    const u32 midRow = kN / 2;
    const size_t mid = at(kN / 2, midRow);
    CHECK(water.depth[mid] > 0.0f);
    CHECK(water.velocityZ[mid] > 0.0f);
    CHECK(water.depth[at(4, midRow)] == 0.0f);
    CHECK(water.depth[at(kN - 5, midRow)] == 0.0f);
    // Downstream carries at least as much water as upstream (rain
    // accumulates along the course).
    CHECK(water.depth[at(kN / 2, kN - 8)] >=
          water.depth[at(kN / 2, 8)] - 0.01f);
    // The sea is pinned to its level.
    const size_t seaCell = at(kN / 2, kN - 2);
    CHECK(ground[seaCell] + water.depth[seaCell] ==
          doctest::Approx(params.seaLevel).epsilon(0.01));

    // Bit-determinism.
    const WaterSolveResult again =
        solveSteadyWater(spec(), ground, params);
    CHECK(water.depth == again.depth);
    CHECK(water.velocityX == again.velocityX);
}
