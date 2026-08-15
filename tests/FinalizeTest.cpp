#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "engine/terrain/generation/Finalize.hpp"

// Stages S5+S6: the fine grid interpolates the macro exactly at
// coincident texels, rivers carve a bed under their water surface, and
// the mask channels land where they should.

using namespace render::terraingen;

namespace {

constexpr u32 kN = 65;
constexpr f32 kTexel = 8.0f;

GridSpec coarse() { return GridSpec { 0.0f, 0.0f, kTexel, kN }; }

size_t at(u32 col, u32 row) {
    return static_cast<size_t>(row) * kN + col;
}

// Tilted V valley along z (as in HydrologyTest) — gives one main river.
vector<f32> valleyHeights() {
    vector<f32> h(coarse().cells());
    const f32 axis = static_cast<f32>(kN / 2);
    for (u32 row = 0; row < kN; ++row) {
        for (u32 col = 0; col < kN; ++col) {
            const f32 lateral =
                std::abs(static_cast<f32>(col) - axis) * 0.8f;
            const f32 down = static_cast<f32>(kN - 1 - row) * 0.3f;
            h[at(col, row)] = 40.0f + lateral + down;
        }
    }
    return h;
}

MacroResult fakeMacro(const vector<f32>& h) {
    MacroResult macro;
    macro.spec = coarse();
    macro.height = h;
    macro.uplift.assign(coarse().cells(), 0.0f);
    macro.biome.assign(coarse().cells(), 0);
    macro.seaDist.assign(coarse().cells(), 5000.0f); // far inland
    return macro;
}

} // namespace

TEST_CASE("upsample matches the macro at coincident texels, deterministic") {
    const auto h = valleyHeights();
    const auto macro = fakeMacro(h);
    const HydrologyParams hp;
    const auto hydro = extractHydrology(coarse(), h, hp);
    FinalizeParams params;
    params.upsampleFactor = 4; // pinned: the checks below assume x4
    params.reliefAmplitude = 0.0f; // isolate the resample
    params.fine.iterations = 0;
    const auto fine = finalizeTerrain(coarse(), h, macro, hydro, coarse(), params, 5);
    CHECK(fine.fineSpec.n == (kN - 1) * 4 + 1);
    CHECK(fine.fineSpec.texelSize == doctest::Approx(2.0f));

    // Away from the river, coincident texels carry the macro heights.
    const auto fineAt = [&](u32 col, u32 row) {
        return fine.height[static_cast<size_t>(row) * fine.fineSpec.n +
                           col];
    };
    CHECK(fineAt(8 * 4, 8 * 4) == doctest::Approx(h[at(8, 8)]));
    CHECK(fineAt(50 * 4, 20 * 4) == doctest::Approx(h[at(50, 20)]));

    const auto fine2 =
        finalizeTerrain(coarse(), h, macro, hydro, coarse(), params, 5);
    CHECK(fine.height == fine2.height); // bit-exact
}

TEST_CASE("the river carves a bed below its water surface") {
    const auto h = valleyHeights();
    const auto macro = fakeMacro(h);
    const HydrologyParams hp;
    const auto hydro = extractHydrology(coarse(), h, hp);
    REQUIRE(!hydro.rivers.empty());
    FinalizeParams params;
    params.fine.iterations = 0; // isolate the carve
    const auto fine = finalizeTerrain(coarse(), h, macro, hydro, coarse(), params, 5);

    const River* main = &hydro.rivers[0];
    for (const River& river : hydro.rivers) {
        if (river.points.size() > main->points.size()) {
            main = &river;
        }
    }
    // Probe mid-river: the bed sits below the water surface; the
    // un-carved valley floor there was AT the surface (routing runs on
    // the terrain), so the carve strictly deepened it.
    const RiverPoint& mid = main->points[main->points.size() / 2];
    const u32 col = static_cast<u32>(
        std::lround((mid.x - fine.fineSpec.originX) /
                    fine.fineSpec.texelSize));
    const u32 row = static_cast<u32>(
        std::lround((mid.z - fine.fineSpec.originZ) /
                    fine.fineSpec.texelSize));
    const f32 bed =
        fine.height[static_cast<size_t>(row) * fine.fineSpec.n + col];
    CHECK(bed < mid.surface - 0.4f);
    // Two half-widths + shoulder away the hillside is untouched.
    const u32 offCol =
        col + static_cast<u32>((mid.halfWidth * 3.0f) /
                               fine.fineSpec.texelSize) +
        4;
    const f32 side =
        fine.height[static_cast<size_t>(row) * fine.fineSpec.n + offCol];
    CHECK(side > mid.surface);
}

TEST_CASE("masks: flow marks the channel, wetness hugs it, detail dies "
          "near water") {
    const auto h = valleyHeights();
    const auto macro = fakeMacro(h);
    const HydrologyParams hp;
    const auto hydro = extractHydrology(coarse(), h, hp);
    FinalizeParams params;
    params.fine.iterations = 0; // isolate the masks
    const auto fine = finalizeTerrain(coarse(), h, macro, hydro, coarse(), params, 5);

    // The valley-axis outlet cell gathered the whole map: flow high.
    const u32 axis = kN / 2;
    CHECK(fine.flow[at(axis, kN - 1)] > 100);
    // A ridge-top corner cell: barely any flow, dry, full detail.
    CHECK(fine.flow[at(2, 2)] < 40);
    CHECK(fine.detailAmp[at(2, 2)] == 255);
    // Wetness at the channel beats the ridge.
    CHECK(fine.wetness[at(axis, kN - 2)] > fine.wetness[at(2, 2)]);
    // No sea nearby: no beach anywhere.
    const u8 maxBeach =
        *std::max_element(fine.beach.begin(), fine.beach.end());
    CHECK(maxBeach == 0);
}

TEST_CASE("rockExposure marks steep bare faces; strata knob is opt-in") {
    // West flat plain, east 45-degree wall.
    vector<f32> h(coarse().cells());
    for (u32 row = 0; row < kN; ++row) {
        for (u32 col = 0; col < kN; ++col) {
            h[at(col, row)] =
                col < 24 ? 40.0f
                         : 40.0f + static_cast<f32>(col - 24) * kTexel;
        }
    }
    const auto macro = fakeMacro(h);
    const HydrologyParams hp;
    const auto hydro = extractHydrology(coarse(), h, hp);
    FinalizeParams params;
    params.fine.iterations = 0;
    vector<f32> deposit(coarse().cells(), 0.0f);
    for (u32 row = 40; row < 50; ++row) {
        for (u32 col = 30; col < 50; ++col) {
            deposit[at(col, row)] = 2.0f; // a scree apron on the wall
        }
    }
    const auto fine = finalizeTerrain(coarse(), h, macro, hydro,
                                      coarse(), params, 5, nullptr,
                                      &deposit);
    REQUIRE(fine.rockExposure.size() == coarse().cells());
    CHECK(fine.rockExposure[at(40, 10)] > 100); // steep bare wall
    CHECK(fine.rockExposure[at(10, 10)] == 0);  // flat plain
    CHECK(fine.rockExposure[at(40, 45)] < 40);  // scree stays covered

    // Geometric strata: default off leaves heights alone; enabled, it
    // displaces the wall but never the plain.
    FinalizeParams strata = params;
    strata.strataAmplitude = 1.0f;
    const auto bent = finalizeTerrain(coarse(), h, macro, hydro,
                                      coarse(), strata, 5, nullptr,
                                      &deposit);
    const auto fineAt = [&](const FinalizeResult& r, u32 col, u32 row) {
        return r.height[static_cast<size_t>(row) * r.fineSpec.n + col];
    };
    CHECK(fineAt(bent, 10 * 4, 10 * 4) == fineAt(fine, 10 * 4, 10 * 4));
    CHECK(bent.height != fine.height);
}

TEST_CASE("beach mask rings the shore of a half-sea macro") {
    // Sea on the west: seaDist ramps from negative (west) to positive.
    auto h = valleyHeights();
    MacroResult macro = fakeMacro(h);
    for (u32 row = 0; row < kN; ++row) {
        for (u32 col = 0; col < kN; ++col) {
            const f32 d =
                (static_cast<f32>(col) - static_cast<f32>(kN / 3)) *
                kTexel;
            macro.seaDist[at(col, row)] = d;
            if (d < 0.0f) {
                h[at(col, row)] = 10.0f; // sea floor
            }
        }
    }
    const HydrologyParams hp;
    const auto hydro = extractHydrology(coarse(), h, hp);
    FinalizeParams params;
    params.fine.iterations = 0; // isolate the beach mask
    const auto fine = finalizeTerrain(coarse(), h, macro, hydro, coarse(), params, 5);
    // On the waterline: full beach. Far inland: none.
    CHECK(fine.beach[at(kN / 3 + 1, 10)] > 200);
    CHECK(fine.beach[at(kN - 3, 10)] == 0);
}

TEST_CASE("lake beds: masked lakes get a shore-profiled basin") {
    // A closed bowl in the plain: the flood makes a masked lake there.
    auto h = valleyHeights();
    for (u32 row = 20; row < 45; ++row) {
        for (u32 col = 15; col < 45; ++col) {
            h[at(col, row)] = 38.0f; // shallow closed pan, spill ~ rim
        }
    }
    const auto macro = fakeMacro(h);
    const HydrologyParams hp;
    const auto hydro = extractHydrology(coarse(), h, hp);
    const Lake* lake = nullptr;
    for (const Lake& candidate : hydro.lakes) {
        if (!candidate.dug && !candidate.mask.empty()) {
            lake = &candidate;
            break;
        }
    }
    REQUIRE(lake != nullptr);

    FinalizeParams params;
    params.fine.iterations = 0;
    const auto fine = finalizeTerrain(coarse(), h, macro, hydro,
                                      coarse(), params, 5);
    // Mid-lake: the bed sits well under the surface now.
    const f32 cx = (lake->minX + lake->maxX) * 0.5f;
    const f32 cz = (lake->minZ + lake->maxZ) * 0.5f;
    const u32 col = static_cast<u32>(
        std::lround((cx - fine.fineSpec.originX) /
                    fine.fineSpec.texelSize));
    const u32 row = static_cast<u32>(
        std::lround((cz - fine.fineSpec.originZ) /
                    fine.fineSpec.texelSize));
    const f32 bed =
        fine.height[static_cast<size_t>(row) * fine.fineSpec.n + col];
    CHECK(bed < lake->level - 2.0f);
    CHECK(bed >= lake->level - params.lakeDepthMax - 0.5f);

    // Disabled coefficient: untouched (the carve is opt-out).
    FinalizeParams flat = params;
    flat.lakeDepthCoef = 0.0f;
    const auto plain = finalizeTerrain(coarse(), h, macro, hydro,
                                       coarse(), flat, 5);
    const f32 plainBed =
        plain.height[static_cast<size_t>(row) * plain.fineSpec.n + col];
    CHECK(plainBed > bed);
}
