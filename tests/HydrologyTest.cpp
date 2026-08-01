#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <numeric>

#include "engine/terrain/generation/Hydrology.hpp"
#include "engine/terrain/generation/ThermalErosion.hpp"

// Stages S3 (thermal talus) and S4 (hydrology extraction): scree obeys
// the angle of repose and conserves mass; lakes surface at their spill
// level; rivers trace downstream and widen as they gather area.

using namespace render::terraingen;

namespace {

constexpr u32 kN = 97;
constexpr f32 kTexel = 8.0f;

GridSpec spec() { return GridSpec { 0.0f, 0.0f, kTexel, kN }; }

size_t at(u32 col, u32 row) {
    return static_cast<size_t>(row) * kN + col;
}

} // namespace

TEST_CASE("thermal erosion relaxes a spike to the talus angle, mass kept") {
    vector<f32> h(spec().cells(), 40.0f);
    h[at(kN / 2, kN / 2)] = 120.0f; // 80 m spike on one texel
    ThermalParams params;
    params.iterations = 120;
    const ThermalResult r = erodeThermal(spec(), h, params);

    // Deterministic.
    const ThermalResult r2 = erodeThermal(spec(), h, params);
    CHECK(r.height == r2.height);

    // The spike relaxed: no neighbouring drop above the repose angle
    // anywhere near the spike.
    const f32 maxDrop = params.talusTan * kTexel * 1.5f; // diagonal slack
    for (u32 row = kN / 2 - 5; row <= kN / 2 + 5; ++row) {
        for (u32 col = kN / 2 - 5; col <= kN / 2 + 5; ++col) {
            const f32 c = r.height[at(col, row)];
            const f32 e = r.height[at(col + 1, row)];
            CHECK(std::abs(c - e) < maxDrop + 0.2f);
        }
    }
    // Mass conservation (interior event, nothing reaches the rim).
    const f64 before = std::accumulate(h.begin(), h.end(), 0.0);
    const f64 after =
        std::accumulate(r.height.begin(), r.height.end(), 0.0);
    CHECK(std::abs(before - after) < 0.01);
    // The slide deposited somewhere next to the spike.
    CHECK(r.deposit[at(kN / 2 + 1, kN / 2)] +
              r.deposit[at(kN / 2 - 1, kN / 2)] +
              r.deposit[at(kN / 2, kN / 2 + 1)] +
              r.deposit[at(kN / 2, kN / 2 - 1)] >
          0.0f);
}

TEST_CASE("a crater above sea level becomes an altitude lake at its spill") {
    // Ring mountain: rim at 90 m, bowl floor at 50 m, one 70 m breach in
    // the rim — the spill. Plain at 40 m.
    vector<f32> h(spec().cells(), 40.0f);
    const f32 cx = static_cast<f32>(kN / 2);
    const f32 cz = static_cast<f32>(kN / 2);
    for (u32 row = 0; row < kN; ++row) {
        for (u32 col = 0; col < kN; ++col) {
            const f32 dx = static_cast<f32>(col) - cx;
            const f32 dz = static_cast<f32>(row) - cz;
            const f32 d = std::sqrt(dx * dx + dz * dz);
            if (d < 6.0f) {
                h[at(col, row)] = 50.0f; // bowl floor
            } else if (d < 10.0f) {
                h[at(col, row)] = 90.0f; // rim
            }
        }
    }
    // Breach the rim eastward at 70 m.
    for (u32 col = kN / 2 + 5; col < kN / 2 + 11; ++col) {
        h[at(col, kN / 2)] = 70.0f;
    }
    const HydrologyParams params;
    const HydrologyResult r = extractHydrology(spec(), h, params);
    REQUIRE(r.lakes.size() == 1);
    const Lake& lake = r.lakes[0];
    // Water rises to the breach (70 m), not the rim (90 m).
    CHECK(lake.level > 69.5f);
    CHECK(lake.level < 71.0f);
    CHECK(lake.cells > 80);
    // The bowl is flooded ~20 m deep.
    CHECK(r.lakeDepth[at(kN / 2, kN / 2)] ==
          doctest::Approx(lake.level - 50.0f).epsilon(0.02));
    // The basin mask marks the bowl center and stays inside the rim:
    // the surface never rides the bbox rectangle.
    REQUIRE(!lake.mask.empty());
    const auto maskAt = [&](f32 wx, f32 wz) {
        const u32 mx = static_cast<u32>(
            std::lround((wx - lake.minX) / lake.maskTexel));
        const u32 mz = static_cast<u32>(
            std::lround((wz - lake.minZ) / lake.maskTexel));
        return lake.mask[static_cast<size_t>(mz) * lake.maskWidth + mx];
    };
    const f32 cxW = static_cast<f32>(kN / 2) * kTexel;
    CHECK(maskAt(cxW, cxW) == 1);
    // A bbox corner (outside the round basin) is dry.
    CHECK(lake.mask[0] == 0);
}

TEST_CASE("a tilted valley traces one widening river to the map edge") {
    // V-shaped valley along z, tilted down toward +z: all drainage
    // funnels into the axis channel.
    vector<f32> h(spec().cells());
    const f32 axis = static_cast<f32>(kN / 2);
    for (u32 row = 0; row < kN; ++row) {
        for (u32 col = 0; col < kN; ++col) {
            const f32 lateral =
                std::abs(static_cast<f32>(col) - axis) * 0.8f;
            const f32 down = static_cast<f32>(kN - 1 - row) * 0.3f;
            h[at(col, row)] = 40.0f + lateral + down;
        }
    }
    const HydrologyParams params;
    const HydrologyResult r = extractHydrology(spec(), h, params);
    // No NATURAL lake in a drained valley; junction PONDS (dug) are
    // allowed where parallel channels merge.
    for (const Lake& lake : r.lakes) {
        CHECK(lake.dug == 1);
    }
    REQUIRE(!r.rivers.empty());
    // The longest river follows the axis and widens downstream.
    const River* main = &r.rivers[0];
    for (const River& river : r.rivers) {
        if (river.points.size() > main->points.size()) {
            main = &river;
        }
    }
    CHECK(main->points.size() > 20);
    for (const RiverPoint& pt : main->points) {
        CHECK(std::abs(pt.x - axis * kTexel) < 2.5f * kTexel);
    }
    CHECK(main->points.back().halfWidth > main->points.front().halfWidth);
    // Downstream surface is monotone non-increasing.
    for (size_t i = 1; i < main->points.size(); ++i) {
        CHECK(main->points[i].surface <=
              main->points[i - 1].surface + 1e-3f);
    }
}
