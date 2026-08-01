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
