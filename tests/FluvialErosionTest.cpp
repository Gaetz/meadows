#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "engine/terrain/generation/FluvialErosion.hpp"

// Stage S2 (fastscape stream power): the implicit solver must be
// deterministic, respect base level, and grow dendritic drainage out of a
// smooth uplift bump — the property that separates eroded ranges from
// noise bumps.

using namespace render::terraingen;

namespace {

constexpr u32 kN = 129;
constexpr f32 kTexel = 8.0f;
constexpr f32 kPlain = 40.0f;

GridSpec spec() { return GridSpec { 0.0f, 0.0f, kTexel, kN }; }

// Flat plain with a centered gaussian uplift field.
vector<f32> flatHeights() {
    return vector<f32>(spec().cells(), kPlain);
}

vector<f32> gaussianUplift() {
    vector<f32> uplift(spec().cells());
    const f32 c = static_cast<f32>(kN - 1) * 0.5f;
    const f32 sigma = static_cast<f32>(kN) * 0.18f;
    for (u32 row = 0; row < kN; ++row) {
        for (u32 col = 0; col < kN; ++col) {
            const f32 dx = static_cast<f32>(col) - c;
            const f32 dz = static_cast<f32>(row) - c;
            uplift[static_cast<size_t>(row) * kN + col] =
                std::exp(-(dx * dx + dz * dz) / (2.0f * sigma * sigma));
        }
    }
    return uplift;
}

} // namespace

TEST_CASE("priority flood fills a closed pit to its spill level") {
    // A 1 m-deep bowl in the middle of the plain, rim at 40 m: the fill
    // must raise the bowl to the spill (+epsilon drainage), not more.
    auto h = flatHeights();
    const u32 c = (kN / 2) * kN + kN / 2;
    h[c] = 30.0f;
    h[c + 1] = 31.0f;
    const auto filled = priorityFloodFill(spec(), h, 21.0f, 1.0e-4f);
    CHECK(filled[c] > kPlain);
    CHECK(filled[c] < kPlain + 0.1f);
    CHECK(filled[c + 1] > kPlain);
    // Untouched terrain fills to (nearly) itself — the epsilon drainage
    // slope accumulates a few millimeters over flat runs, by design.
    CHECK(filled[10 * kN + 10] >= kPlain);
    CHECK(filled[10 * kN + 10] < kPlain + 0.05f);
}

TEST_CASE("fastscape is deterministic and identity at zero k and uplift") {
    FluvialParams params;
    params.iterations = 20;
    const auto h = flatHeights();
    const auto uplift = gaussianUplift();
    const FluvialResult a = erodeFluvial(spec(), h, uplift, params);
    const FluvialResult b = erodeFluvial(spec(), h, uplift, params);
    CHECK(a.height == b.height); // bit-exact

    FluvialParams inert;
    inert.iterations = 20;
    inert.k = 0.0f;
    inert.upliftRate = 0.0f;
    const vector<f32> zero(spec().cells(), 0.0f);
    const FluvialResult c = erodeFluvial(spec(), h, zero, inert);
    CHECK(c.height == h);
}

TEST_CASE("uplift bump erodes into a drained dendritic range") {
    FluvialParams params;
    const auto h = flatHeights();
    const auto uplift = gaussianUplift();
    const FluvialResult r = erodeFluvial(spec(), h, uplift, params);

    // The rim is base level: untouched.
    for (u32 col = 0; col < kN; ++col) {
        CHECK(r.height[col] == kPlain);
        CHECK(r.height[static_cast<size_t>(kN - 1) * kN + col] == kPlain);
    }
    // The range rose under the bump...
    f32 peak = 0.0f;
    for (const f32 v : r.height) {
        peak = std::max(peak, v);
    }
    CHECK(peak > kPlain + 25.0f);
    // ...but stream power kept it from the pure-uplift ceiling.
    CHECK(peak < kPlain + params.upliftRate *
                              static_cast<f32>(params.iterations));

    // Dendritic drainage: some collector gathered a real catchment.
    const f32 cellArea = kTexel * kTexel;
    f32 maxArea = 0.0f;
    u32 channels = 0;
    for (const f32 a : r.area) {
        maxArea = std::max(maxArea, a);
        if (a > 64.0f * cellArea) {
            ++channels;
        }
    }
    // A radial bump drains through ~10 major valleys; the biggest one
    // must have captured a real catchment (hundreds of cells).
    CHECK(maxArea > 300.0f * cellArea);
    CHECK(channels > 50);

    // The final surface drains: closed pits are rare and shallow.
    const auto filled =
        priorityFloodFill(spec(), r.height, params.seaLevel,
                          params.minSlope);
    u32 deepPits = 0;
    for (size_t i = 0; i < filled.size(); ++i) {
        if (filled[i] - r.height[i] > 0.5f) {
            ++deepPits;
        }
    }
    CHECK(deepPits < spec().cells() / 50);
}

TEST_CASE("routeFlow: receivers descend, order ascends, area accumulates") {
    // Plane tilted toward +z: interior flow runs straight down columns.
    vector<f32> h(spec().cells());
    for (u32 row = 0; row < kN; ++row) {
        for (u32 col = 0; col < kN; ++col) {
            h[static_cast<size_t>(row) * kN + col] =
                100.0f - static_cast<f32>(row) * 0.5f;
        }
    }
    const auto filled = priorityFloodFill(spec(), h, 0.0f, 1.0e-4f);
    const FlowRouting routing = routeFlow(spec(), filled, h, 0.0f);

    // Base-level cells receive themselves; everything else routes to a
    // strictly lower cell of the routed surface.
    u32 baseCells = 0;
    u32 badReceivers = 0;
    for (u32 i = 0; i < spec().cells(); ++i) {
        if (routing.receiver[i] == i) {
            ++baseCells;
            continue;
        }
        if (filled[routing.receiver[i]] >= filled[i]) {
            ++badReceivers;
        }
    }
    CHECK(badReceivers == 0);
    CHECK(baseCells >= 4 * kN - 4); // at least the rim

    // Process order sorts the routed surface ascending.
    u32 orderViolations = 0;
    for (size_t k = 1; k < routing.order.size(); ++k) {
        if (filled[routing.order[k]] < filled[routing.order[k - 1]]) {
            ++orderViolations;
        }
    }
    CHECK(orderViolations == 0);

    // A near-outlet interior cell gathered its whole upstream column.
    const f32 cellArea = kTexel * kTexel;
    const size_t nearOutlet =
        static_cast<size_t>(kN - 2) * kN + kN / 2;
    CHECK(routing.area[nearOutlet] ==
          doctest::Approx(static_cast<f32>(kN - 2) * cellArea)
              .epsilon(0.01));
}

TEST_CASE("plateauKeep re-blends mesas out of the dissection") {
    FluvialParams params;
    params.iterations = 40;
    const auto h = flatHeights();
    const auto uplift = gaussianUplift();
    vector<f32> keep(spec().cells(), 1.0f); // full keep: S1 wins
    const FluvialResult kept =
        erodeFluvial(spec(), h, uplift, params, &keep);
    CHECK(kept.height == h);

    vector<f32> half(spec().cells(), 0.5f);
    const FluvialResult blended =
        erodeFluvial(spec(), h, uplift, params, &half);
    const FluvialResult free = erodeFluvial(spec(), h, uplift, params);
    // Halfway between input and free-running erosion, texel-wise.
    const u32 c = (kN / 2) * kN + kN / 2;
    CHECK(blended.height[c] ==
          doctest::Approx((free.height[c] + kPlain) * 0.5f)
              .epsilon(0.05));
}

TEST_CASE("sediment transport: flats fill and flatten, peaks stand, "
          "drainage survives") {
    FluvialParams off;
    off.iterations = 60;
    off.sedimentCapacity = 0.0f;
    FluvialParams on = off;
    on.sedimentCapacity = 1.2f;
    const auto h = flatHeights();
    const auto uplift = gaussianUplift();

    const FluvialResult dry = erodeFluvial(spec(), h, uplift, off);
    CHECK(dry.deposit.empty()); // 0 = transport-unlimited legacy path

    const FluvialResult wet = erodeFluvial(spec(), h, uplift, on);
    const FluvialResult wet2 = erodeFluvial(spec(), h, uplift, on);
    CHECK(wet.height == wet2.height); // deterministic, bit-exact
    REQUIRE(wet.deposit.size() == spec().cells());

    // Sediment actually lands.
    f32 total = 0.0f;
    for (const f32 d : wet.deposit) {
        total += d;
    }
    CHECK(total > 1.0f);

    // Peaks are transport-immune: steep slopes carry everything away.
    const auto peakOf = [](const vector<f32>& grid) {
        f32 peak = 0.0f;
        for (const f32 v : grid) {
            peak = std::max(peak, v);
        }
        return peak;
    };
    CHECK(std::abs(peakOf(wet.height) - peakOf(dry.height)) < 2.0f);

    // Where sediment landed, the floor is FLATTER than the dry carve.
    const auto meanSlopeOver = [&](const vector<f32>& grid) {
        f64 sum = 0.0;
        u32 count = 0;
        for (u32 row = 1; row + 1 < kN; ++row) {
            for (u32 col = 1; col + 1 < kN; ++col) {
                const size_t i = static_cast<size_t>(row) * kN + col;
                if (wet.deposit[i] < 0.2f) {
                    continue;
                }
                const f32 gx = (grid[i + 1] - grid[i - 1]) /
                               (2.0f * kTexel);
                const f32 gz = (grid[i + kN] - grid[i - kN]) /
                               (2.0f * kTexel);
                sum += std::sqrt(static_cast<f64>(gx * gx + gz * gz));
                ++count;
            }
        }
        return count ? sum / count : 0.0;
    };
    CHECK(meanSlopeOver(wet.height) < meanSlopeOver(dry.height));

    // Deposition never breaks the drained invariant: pits stay rare and
    // shallow (the routed-surface clamp at work).
    const auto filled = priorityFloodFill(spec(), wet.height, on.seaLevel,
                                          on.minSlope);
    u32 deepPits = 0;
    for (size_t i = 0; i < filled.size(); ++i) {
        if (filled[i] - wet.height[i] > 0.5f) {
            ++deepPits;
        }
    }
    CHECK(deepPits < spec().cells() / 50);
}

TEST_CASE("erodibility scales the incision") {
    FluvialParams params;
    params.iterations = 40;
    const auto h = flatHeights();
    const auto uplift = gaussianUplift();
    const FluvialResult base = erodeFluvial(spec(), h, uplift, params);
    const vector<f32> soft(spec().cells(), 1.5f);
    const FluvialResult eroded =
        erodeFluvial(spec(), h, uplift, params, nullptr, &soft);
    const auto peakOf = [](const vector<f32>& grid) {
        f32 peak = 0.0f;
        for (const f32 v : grid) {
            peak = std::max(peak, v);
        }
        return peak;
    };
    // Softer rock (k x1.5) cannot hold the same range against the same
    // uplift: the equilibrium peak sits lower.
    CHECK(peakOf(eroded.height) < peakOf(base.height) - 1.0f);
}

TEST_CASE("sea nodes are base level: never uplifted, never carved below") {
    FluvialParams params;
    params.iterations = 30;
    auto h = flatHeights();
    // West third is sea floor.
    for (u32 row = 0; row < kN; ++row) {
        for (u32 col = 0; col < kN / 3; ++col) {
            h[static_cast<size_t>(row) * kN + col] = 10.0f;
        }
    }
    const auto uplift = gaussianUplift();
    const FluvialResult r = erodeFluvial(spec(), h, uplift, params);
    for (u32 row = 0; row < kN; ++row) {
        for (u32 col = 0; col < kN / 3; ++col) {
            CHECK(r.height[static_cast<size_t>(row) * kN + col] == 10.0f);
        }
    }
}

TEST_CASE("sediment keeps a floor under deep flooded cells") {
    // Deep closed bowl fed by the eroding bump: sediment pours in but
    // may never pave the basin up to its water surface.
    FluvialParams params;
    params.iterations = 80;
    auto h = flatHeights();
    for (u32 row = 8; row < 24; ++row) {
        for (u32 col = 8; col < 24; ++col) {
            h[static_cast<size_t>(row) * kN + col] =
                20.0f; // 20 m-deep pan near the bump
        }
    }
    const auto uplift = gaussianUplift();
    const FluvialResult r = erodeFluvial(spec(), h, uplift, params);
    const auto filled = priorityFloodFill(spec(), r.height,
                                          params.seaLevel,
                                          params.minSlope);
    u32 violations = 0;
    for (u32 row = 9; row < 23; ++row) {
        for (u32 col = 9; col < 23; ++col) {
            const size_t i = static_cast<size_t>(row) * kN + col;
            if (r.deposit.empty() || r.deposit[i] <= 0.0f) {
                continue;
            }
            const f32 depth = filled[i] - r.height[i];
            // Cells the sediment touched and that stay flooded keep
            // (about) the guaranteed floor.
            if (depth > 0.5f && depth < params.lakeKeepDepth - 0.5f) {
                ++violations;
            }
        }
    }
    CHECK(violations == 0);
}
