#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "engine/terrain/Noise.hpp"
#include "engine/terrain/generation/FineErosion.hpp"

// The fine-erosion amplification must be deterministic, budget-bounded,
// protection-aware and — the property the whole tile system leans on —
// STRICTLY LOCAL: terrain changes beyond its support radius cannot move
// a single bit of the output.

using namespace render::terraingen;

namespace {

constexpr u32 kN = 193;
constexpr f32 kTexel = 4.0f;

GridSpec spec() { return GridSpec { 0.0f, 0.0f, kTexel, kN }; }

// Open hillside: a tilted plane with deterministic noise bumps.
vector<f32> hillside() {
    vector<f32> h(spec().cells());
    for (u32 row = 0; row < kN; ++row) {
        for (u32 col = 0; col < kN; ++col) {
            const f32 x = static_cast<f32>(col) * kTexel;
            const f32 z = static_cast<f32>(row) * kTexel;
            h[static_cast<size_t>(row) * kN + col] =
                120.0f - z * 0.25f +
                (render::noise::fbm(77u, x, z, 1.0f / 90.0f, 3, 2.0f,
                                    0.5f) *
                     2.0f -
                 1.0f) *
                    9.0f;
        }
    }
    return h;
}

} // namespace

TEST_CASE("fine erosion: deterministic, budgeted, and off when asked") {
    const auto h = hillside();
    const vector<f32> allow(spec().cells(), 1.0f);
    const vector<f32> discharge(spec().cells(), 0.3f);
    const FineErosionParams params;

    const FineErosionResult a =
        amplifyFine(spec(), h, allow, discharge, params);
    const FineErosionResult b =
        amplifyFine(spec(), h, allow, discharge, params);
    CHECK(a.height == b.height); // bit-exact

    // It actually carves, and every texel honors the depth budget.
    f32 total = 0.0f;
    f32 deepest = 0.0f;
    for (const f32 v : a.incision) {
        total += v;
        deepest = std::max(deepest, v);
        CHECK(v <= params.maxDepth + 1.0e-4f);
    }
    CHECK(total > 50.0f);
    CHECK(deepest > 0.5f);

    FineErosionParams off = params;
    off.iterations = 0;
    const FineErosionResult none =
        amplifyFine(spec(), h, allow, discharge, off);
    CHECK(none.height == h);
}

TEST_CASE("fine erosion: protection zeroes the carve") {
    const auto h = hillside();
    vector<f32> allow(spec().cells(), 1.0f);
    // Protect a band of rows (a river corridor, say).
    for (u32 row = 80; row < 100; ++row) {
        for (u32 col = 0; col < kN; ++col) {
            allow[static_cast<size_t>(row) * kN + col] = 0.0f;
        }
    }
    const vector<f32> discharge(spec().cells(), 0.3f);
    const FineErosionResult r =
        amplifyFine(spec(), h, allow, discharge, FineErosionParams {});
    for (u32 row = 80; row < 100; ++row) {
        for (u32 col = 0; col < kN; ++col) {
            const size_t i = static_cast<size_t>(row) * kN + col;
            CHECK(r.incision[i] == 0.0f);
            CHECK(r.height[i] == h[i]);
        }
    }
}

TEST_CASE("fine erosion is local: far perturbations change nothing") {
    const auto h = hillside();
    const vector<f32> allow(spec().cells(), 1.0f);
    const vector<f32> discharge(spec().cells(), 0.3f);
    const FineErosionParams params;

    // Support radius: accumulation reach + one texel of receiver drift
    // per iteration. Perturb everything beyond it (east side) and
    // compare the west probe region bit for bit.
    const f32 support =
        static_cast<f32>(params.reachSteps) * kTexel +
        static_cast<f32>(params.iterations) * kTexel * 1.5f;
    const u32 perturbCol = 130;
    const u32 probeMax =
        perturbCol - static_cast<u32>(support / kTexel) - 2;
    REQUIRE(probeMax > 10); // the test still probes something real

    auto bumped = h;
    for (u32 row = 0; row < kN; ++row) {
        for (u32 col = perturbCol; col < kN; ++col) {
            bumped[static_cast<size_t>(row) * kN + col] += 25.0f;
        }
    }
    const FineErosionResult a =
        amplifyFine(spec(), h, allow, discharge, params);
    const FineErosionResult b =
        amplifyFine(spec(), bumped, allow, discharge, params);
    for (u32 row = 0; row < kN; ++row) {
        for (u32 col = 0; col < probeMax; ++col) {
            const size_t i = static_cast<size_t>(row) * kN + col;
            REQUIRE(a.height[i] == b.height[i]); // bit-exact locality
        }
    }
}

TEST_CASE("discharge coupling deepens the carve") {
    const auto h = hillside();
    const vector<f32> allow(spec().cells(), 1.0f);
    const vector<f32> dry(spec().cells(), 0.0f);
    const vector<f32> wet(spec().cells(), 1.0f);
    const FineErosionParams params;
    const FineErosionResult a = amplifyFine(spec(), h, allow, dry, params);
    const FineErosionResult b = amplifyFine(spec(), h, allow, wet, params);
    f32 totalDry = 0.0f;
    f32 totalWet = 0.0f;
    for (size_t i = 0; i < a.incision.size(); ++i) {
        totalDry += a.incision[i];
        totalWet += b.incision[i];
    }
    CHECK(totalWet > totalDry * 1.2f);
}
