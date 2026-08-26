#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <numeric>

#include "engine/terrain/generation/Hydrology.hpp"
#include "engine/terrain/generation/MasterNetwork.hpp"
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

TEST_CASE("ridge rounding shaves peaks, spares flanks and valleys") {
    const u32 c = kN / 2;
    const auto dist = [&](u32 col, u32 row) {
        const f32 dx = (static_cast<f32>(col) - static_cast<f32>(c)) * kTexel;
        const f32 dz = (static_cast<f32>(row) - static_cast<f32>(c)) * kTexel;
        return std::sqrt(dx * dx + dz * dz);
    };
    // A sharp cone: apex at 100 m, planar flanks.
    vector<f32> cone(spec().cells());
    for (u32 row = 0; row < kN; ++row) {
        for (u32 col = 0; col < kN; ++col) {
            cone[at(col, row)] = 100.0f - 0.3f * dist(col, row);
        }
    }
    const RidgeRoundParams params;
    const auto rounded = roundRidges(spec(), cone, params);

    // Deterministic; the apex lost meters, a mid-flank point (whose
    // convexity sits under the prominence band) kept its height.
    CHECK(rounded == roundRidges(spec(), cone, params));
    CHECK(cone[at(c, c)] - rounded[at(c, c)] > 5.0f);
    CHECK(std::abs(cone[at(c + 20, c)] - rounded[at(c + 20, c)]) < 0.5f);

    // An inverted cone (a valley) is concave: its floor never moves.
    vector<f32> valley(spec().cells());
    for (u32 row = 0; row < kN; ++row) {
        for (u32 col = 0; col < kN; ++col) {
            valley[at(col, row)] = 30.0f + 0.3f * dist(col, row);
        }
    }
    const auto vRounded = roundRidges(spec(), valley, params);
    CHECK(vRounded[at(c, c)] == valley[at(c, c)]);

    // Off switch and zero weight are both bit-exact.
    RidgeRoundParams off = params;
    off.strength = 0.0f;
    CHECK(roundRidges(spec(), cone, off) == cone);
    const vector<f32> zeros(spec().cells(), 0.0f);
    CHECK(roundRidges(spec(), cone, params, &zeros) == cone);
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

TEST_CASE("a rejected pothole does not interrupt the river") {
    // Tilted valley with a small deep dip on the axis: too few cells to
    // be a lake, deep enough to flood — the trace must ride the spill
    // surface THROUGH it (one continuous course, monotone surface)
    // instead of breaking into fragments around a dry gap.
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
    // The pothole: 2x2 cells, 3 m deep, mid-course on the axis.
    const u32 ax = kN / 2;
    for (u32 row = kN / 2; row < kN / 2 + 2; ++row) {
        for (u32 col = ax; col < ax + 2; ++col) {
            h[at(col, row)] -= 3.0f;
        }
    }
    HydrologyParams params;
    params.riverArea = 60000.0f; // the tiny grid's channel rhythm
    const HydrologyResult r = extractHydrology(spec(), h, params);
    // No accepted lake out of a 2x2 dip.
    for (const Lake& lake : r.lakes) {
        CHECK(lake.dug == 1);
    }
    // One course crosses the pothole row: some river holds points both
    // well above and well below it.
    const f32 potholeZ = spec().z(kN / 2);
    bool crosses = false;
    for (const River& river : r.rivers) {
        f32 minZ = 1.0e9f;
        f32 maxZ = -1.0e9f;
        for (const RiverPoint& pt : river.points) {
            minZ = glm::min(minZ, pt.z);
            maxZ = glm::max(maxZ, pt.z);
        }
        if (minZ < potholeZ - 60.0f && maxZ > potholeZ + 60.0f) {
            crosses = true;
            // And its surface stays monotone through the dip.
            for (size_t i = 1; i < river.points.size(); ++i) {
                CHECK(river.points[i].surface <=
                      river.points[i - 1].surface + 1e-3f);
            }
        }
    }
    CHECK(crosses);
}

TEST_CASE("river tiers: area threshold, fleuve promotion, ford grid") {
    HydrologyParams params;
    const auto course = [](f32 x0, f32 length, f32 area) {
        River r;
        r.mouthArea = area;
        for (f32 d = 0.0f; d <= length; d += 25.0f) {
            RiverPoint pt;
            pt.x = x0;
            pt.z = d;
            pt.surface = 100.0f - d * 0.01f;
            pt.halfWidth = 3.0f;
            r.points.push_back(pt);
        }
        return r;
    };
    const auto build = [&] {
        vector<River> rivers;
        rivers.push_back(course(0.0f, 500.0f, 3.0e5f));      // ruisseau
        rivers.push_back(course(5000.0f, 6000.0f, 3.0e6f));  // rivière
        rivers.push_back(course(20000.0f, 4000.0f, 4.0e6f)); // fleuve
        return rivers;
    };
    // Master course aligned with the third river, areas growing
    // downstream past the fleuve grade.
    MasterRiver masterRiver;
    for (f32 d = 0.0f; d <= 4000.0f; d += 128.0f) {
        masterRiver.nodes.push_back(
            { 20030.0f, d, 90.0f, 7.0e6f + d * 2000.0f });
    }
    const vector<MasterRiver> master { masterRiver };

    vector<River> rivers = build();
    classifyRivers(rivers, params, 1337, master);
    // Ruisseau: below the rivière area, no fords.
    CHECK(rivers[0].tier == 0);
    CHECK(rivers[0].fords.empty());
    // Rivière: fords at the grid rhythm (~one per fordSpacing of
    // course), each ON the course.
    CHECK(rivers[1].tier == 1);
    CHECK(rivers[1].fords.size() >= 2);
    CHECK(rivers[1].fords.size() <= 8);
    for (const Vec2& ford : rivers[1].fords) {
        CHECK(std::abs(ford.x - 5000.0f) < 1.0f);
    }
    // Fleuve: promoted by the master match, width floor >= 12 m and
    // monotone downstream, no fords (the obstacle tier).
    CHECK(rivers[2].tier == 2);
    CHECK(rivers[2].fords.empty());
    CHECK(rivers[2].points.back().halfWidth >= 12.0f);
    f32 prev = 0.0f;
    for (const RiverPoint& pt : rivers[2].points) {
        CHECK(pt.halfWidth >= prev - 1.0e-3f);
        prev = pt.halfWidth;
    }
    // Determinism: same inputs, same fords.
    vector<River> again = build();
    classifyRivers(again, params, 1337, master);
    REQUIRE(again[1].fords.size() == rivers[1].fords.size());
    for (size_t f = 0; f < again[1].fords.size(); ++f) {
        CHECK(again[1].fords[f].x == rivers[1].fords[f].x);
        CHECK(again[1].fords[f].y == rivers[1].fords[f].y);
    }
    // No master in sight: the big course is a plain rivière.
    vector<River> alone = build();
    classifyRivers(alone, params, 1337, {});
    CHECK(alone[2].tier == 1);
}
