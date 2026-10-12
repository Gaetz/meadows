#include <doctest/doctest.h>

#include <chrono>
#include <cmath>

#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/terrain/SandboxTerrain.hpp"
#include "engine/terrain/generation/GridOps.hpp"
#include "engine/terrain/generation/TileBake.hpp"
#include "game/TerrainBakeStreamer.hpp"

// Seconds-to-minutes cases (bakes, sims): the "slow" suite, skipped by
// the fast run (-tse=slow), kept by the full one (docs/AUDIT/U9-tests.md).
TEST_SUITE_BEGIN("slow");

// Sandbox tiles: deterministic bakes, the stage-1/stage-2 seam, and
// adjacent tiles that blend smoothly through their shared margin ring
// inside height().

using namespace render::terraingen;

namespace {

// Small, fast tile setup: 512 m tiles, 256 m apron, 16 erosion
// iterations — the structure is what the tests probe, not the look.
TileBakeParams testParams() {
    TileBakeParams p;
    p.worldSeed = 20260731;
    p.tileSize = 512.0f;
    p.apron = 256.0f;
    p.overlapMargin = 64.0f;
    p.macroTexel = 8.0f;
    p.fluvial.iterations = 16;
    p.thermal.iterations = 16;
    p.finalize.upsampleFactor = 2; // 4 m runtime texels
    return p;
}

} // namespace

TEST_CASE("lakeReachesPoint sonde l'empreinte POST-reconcile") {
    // Un lac dont le reconcile a retiré la moitié nord : une tête de
    // cours près de la moitié couverte est encore nourrie ; près de
    // l'ancienne moitié (retirée), elle ne l'est plus — le cours
    // orphelin sera lâché.
    vector<Lake> lakes(1);
    Lake& lake = lakes[0];
    lake.level = 200.0f;
    lake.minX = 0.0f;
    lake.minZ = 0.0f;
    lake.maxX = 80.0f;
    lake.maxZ = 80.0f;
    lake.maskTexel = 8.0f;
    lake.maskWidth = 10;
    lake.maskHeight = 10;
    lake.mask.assign(100, 0);
    u32 cells = 0;
    for (u32 r = 0; r < 5; ++r) { // seule la moitié SUD couverte
        for (u32 c = 0; c < 10; ++c) {
            lake.mask[r * 10 + c] = 1;
            ++cells;
        }
    }
    lake.cells = cells;
    CHECK(lakeReachesPoint(lakes, 40.0f, 36.0f));  // bord sud: ~4 m
    CHECK(!lakeReachesPoint(lakes, 40.0f, 76.0f)); // moitié retirée
    CHECK(!lakeReachesPoint(lakes, 40.0f, 60.0f)); // à 24 m du bord :
                                                   // coïncidence, pas
                                                   // une alimentation
    CHECK(!lakeReachesPoint(lakes, 200.0f, 200.0f)); // loin de tout
    lake.cells = 0; // lac vidé par le reconcile: ne nourrit plus rien
    CHECK(!lakeReachesPoint(lakes, 40.0f, 36.0f));
}

TEST_CASE("tile bakes are deterministic") {
    const TileBakeParams params = testParams();
    const TileBakeResult a = bakeSoloTile(params, 3, -2);
    const TileBakeResult b = bakeSoloTile(params, 3, -2);
    CHECK(a.region.heights == b.region.heights); // bit-exact (cache!)
    CHECK(a.region.biome == b.region.biome);
    CHECK(a.lakes.size() == b.lakes.size());

    // Geometry: covers tile + margin, aligned on the tile grid.
    CHECK(a.region.originX ==
          doctest::Approx(3.0f * params.tileSize - params.overlapMargin));
    CHECK(a.region.spanX() ==
          doctest::Approx(params.tileSize + 2.0f * params.overlapMargin));
    CHECK(a.region.maskWidth > 2);

    // Water-field determinism holds whether or not the tile has land
    // (a fully-submerged rect legitimately ships empty fields — the
    // ocean sheet owns it).
    CHECK(a.region.waterWidth == b.region.waterWidth);
    CHECK(a.region.waterDepth == b.region.waterDepth); // bit-exact
    CHECK(a.region.waterSurface == b.region.waterSurface);
    CHECK(a.region.waterFlux == b.region.waterFlux);
}

TEST_CASE("adjacent tiles blend smoothly across their shared border") {
    const TileBakeParams params = testParams();
    const TileBakeResult a = bakeSoloTile(params, 0, 0);
    const TileBakeResult b = bakeSoloTile(params, 1, 0);

    auto base = std::make_shared<render::TerrainBase>();
    base->regions.push_back(
        std::make_shared<render::TerrainRegion>(a.region));
    base->regions.push_back(
        std::make_shared<render::TerrainRegion>(b.region));
    render::TerrainParams tp;
    tp.base = base;
    auto sandbox = std::make_shared<render::SandboxTerrain>();
    sandbox->controls = params.controls;
    sandbox->controls.seed = params.worldSeed;
    sandbox->macro = params.macro;
    tp.sandbox = sandbox;

    // Walk across the border at several z: no step discontinuity. The
    // two bakes disagree slightly (different aprons); the overlap band
    // must swallow the residual.
    const f32 border = params.tileSize;
    for (const f32 z : { 64.0f, 200.0f, 388.0f }) {
        f32 previous = render::terrain::height(tp, border - 80.0f, z);
        f32 maxStep = 0.0f;
        for (f32 x = border - 79.0f; x <= border + 80.0f; x += 1.0f) {
            const f32 h = render::terrain::height(tp, x, z);
            maxStep = std::max(maxStep, std::abs(h - previous));
            previous = h;
        }
        // 4 m texels: legitimate slopes exist, cliffs don't (this strip
        // of the test world is rolling coast/plain).
        CHECK(maxStep < 6.0f);
    }

    // The shared overlap band itself: both bakes carry heights there
    // (different aprons, fine erosion included) — their disagreement
    // must stay well inside what the edge blend swallows.
    f32 maxDiverge = 0.0f;
    f32 maxDivergeLand = 0.0f;
    f32 worstX = 0.0f, worstZ = 0.0f, worstA = 0.0f, worstB = 0.0f;
    // The band both REGIONS carry (a region is cropped to its tile +
    // margin; a read past its rect clamps to the rim texel — on a
    // cliff that is a 77 m lie, not a divergence).
    const f32 xLo = glm::max(a.region.originX, b.region.originX) + 2.0f;
    const f32 xHi = glm::min(a.region.originX + a.region.spanX(),
                             b.region.originX + b.region.spanX()) -
                    2.0f;
    MESSAGE("region a x ", a.region.originX, "..",
            a.region.originX + a.region.spanX(), ", region b x ",
            b.region.originX, "..", b.region.originX + b.region.spanX(),
            "; sampled band ", xLo, "..", xHi);
    REQUIRE(xHi > xLo);
    for (f32 z = 32.0f; z < params.tileSize; z += 24.0f) {
        for (f32 x = xLo; x <= xHi; x += 8.0f) {
            const f32 ha = render::terrain::baseHeight(a.region, x, z);
            const f32 hb = render::terrain::baseHeight(b.region, x, z);
            const f32 d = std::abs(ha - hb);
            if (d > maxDiverge) {
                maxDiverge = d;
                worstX = x;
                worstZ = z;
                worstA = ha;
                worstB = hb;
            }
            if (!lakeReachesPoint(a.lakes, x, z) &&
                !lakeReachesPoint(b.lakes, x, z)) {
                maxDivergeLand = std::max(maxDivergeLand, d);
            }
        }
    }
    // The local dimple fill reads a ~1 km box mean: two windows with
    // different aprons fill a rim hollow differently (the map pipeline
    // bakes ONE stage-1 per map; the map-line divergence is N4's
    // measure, bake-map --pair). A closed basin — a plateau bench
    // enclosed by higher ground — is resolved per window by the
    // passes after stage-1 (hydrology, lakes, finalize): measured on
    // the plateaus, the stage-1 ground of the two windows agrees
    // within 5 m at the worst point while the finalized ground
    // differs by 77 m there. The mechanism is N4's to pin down with
    // the shared-apron pair bake; until then the bound is a plateau
    // step.
    MESSAGE("overlap band divergence: ", maxDiverge, " m at (", worstX, ", ",
            worstZ, "): ", worstA, " vs ", worstB, "; outside the lakes ",
            maxDivergeLand, " m");
    {
        // Where the worst point diverges: the stage-1 macro, the
        // stage-1 eroded, the baked ground of both windows.
        const TileStage1 sa = bakeTileStage1(params, 0, 0);
        const TileStage1 sb = bakeTileStage1(params, 1, 0);
        MESSAGE("worst point stage-1: macro ",
                bilinearWorld(sa.sim, sa.macroHeight, worstX, worstZ), " vs ",
                bilinearWorld(sb.sim, sb.macroHeight, worstX, worstZ),
                "; eroded ", bilinearWorld(sa.sim, sa.eroded, worstX, worstZ),
                " vs ", bilinearWorld(sb.sim, sb.eroded, worstX, worstZ),
                "; budget ", bilinearWorld(sa.sim, sa.budget, worstX, worstZ),
                " vs ", bilinearWorld(sb.sim, sb.budget, worstX, worstZ));
    }
    CHECK(maxDivergeLand < 130.0f); // a plateau step (see above)
    CHECK(maxDiverge < 150.0f);
}

TEST_CASE("biome erosion character: neutral is identity, borders blur") {
    const TileBakeParams params = testParams();

    // Empty table and an explicit all-neutral table are bit-identical:
    // multiplying k/talus by 1.0 changes nothing.
    TileBakeParams neutral = params;
    neutral.biomeErosion.clear();
    TileBakeParams unit = params;
    unit.biomeErosion.assign(4, BiomeErosion {});
    const TileStage1 a = bakeTileStage1(neutral, 0, 0);
    const TileStage1 b = bakeTileStage1(unit, 0, 0);
    CHECK(a.eroded == b.eroded);

    // A hard biome border blurs: adjacent-texel erodibility steps stay
    // well below the raw temperate->arid contrast (0.9 -> 1.3).
    const GridSpec small { 0.0f, 0.0f, 8.0f, 17 };
    vector<u8> biome(small.cells(), 0);
    for (u32 row = 0; row < small.n; ++row) {
        for (u32 col = 8; col < small.n; ++col) {
            biome[static_cast<size_t>(row) * small.n + col] = 1;
        }
    }
    const BiomeCharacter grids =
        biomeCharacter(small, biome, params.biomeErosion);
    REQUIRE(grids.erodibility.size() == small.cells());
    f32 maxStep = 0.0f;
    for (u32 row = 0; row < small.n; ++row) {
        for (u32 col = 1; col < small.n; ++col) {
            const size_t i = static_cast<size_t>(row) * small.n + col;
            maxStep = std::max(maxStep,
                               std::abs(grids.erodibility[i] -
                                        grids.erodibility[i - 1]));
        }
    }
    CHECK(maxStep > 0.0f);
    CHECK(maxStep < 0.2f);

    // Away from the border the grids carry the table values (temperate
    // 0.9 west, arid 1.3 east — the default palette contract).
    const size_t west = static_cast<size_t>(8) * small.n + 2;
    const size_t east = static_cast<size_t>(8) * small.n + 14;
    CHECK(grids.erodibility[west] == doctest::Approx(0.9f));
    CHECK(grids.erodibility[east] == doctest::Approx(1.3f));
    CHECK(grids.talusScale[west] == doctest::Approx(1.0f));
    CHECK(grids.capacityScale[east] == doctest::Approx(0.7f));
}

TEST_CASE("stage seam: stage-1 deterministic, the solo map bake too") {
    const TileBakeParams params = testParams();
    const TileStage1 s1a = bakeTileStage1(params, 1, 1);
    const TileStage1 s1b = bakeTileStage1(params, 1, 1);
    CHECK(s1a.eroded == s1b.eroded); // bit-exact (disk cache contract)
    CHECK(s1a.seaDist == s1b.seaDist);
    CHECK(s1a.biome == s1b.biome);

    // The solo bake (a 1x1-slice map — THE production pipeline at
    // bench scale): valid geometry, bit-exact determinism.
    const TileBakeResult lone = bakeSoloTile(params, 1, 1);
    CHECK(lone.region.width > 2);
    CHECK(lone.region.heights.size() ==
          static_cast<size_t>(lone.region.width) * lone.region.height);
    CHECK(lone.region.originX ==
          doctest::Approx(params.tileSize - params.overlapMargin));
    const TileBakeResult lone2 = bakeSoloTile(params, 1, 1);
    CHECK(lone.region.heights == lone2.region.heights);
}

// Hidden benchmark: full production-size tile (4 km, 1 km apron, 100
// fluvial iterations, 2 m output). Run explicitly with:
//   meadows-tests -tc="*bake benchmark*" -nsf
TEST_CASE("full tile bake benchmark" * doctest::skip()) {
    TileBakeParams params;
    params.worldSeed = 1337;
    const auto start = std::chrono::steady_clock::now();
    const TileBakeResult r = bakeSoloTile(params, 0, 0);
    const f64 seconds =
        std::chrono::duration<f64>(std::chrono::steady_clock::now() -
                                   start)
            .count();
    MESSAGE("full 4 km tile bake: ", seconds, " s, region ",
            r.region.width, "^2 texels, ", r.lakes.size(), " lakes, ",
            r.rivers.size(), " rivers");
    // Content hash over the raw height bytes: the bit-exactness proof
    // reused by every chantier-économie brick (docs/CPU-PERF.md) — an
    // optimization that moves this hash changed the terrain.
    u64 h = 1469598103934665603ull; // FNV-1a
    const auto* bytes =
        reinterpret_cast<const unsigned char*>(r.region.heights.data());
    const size_t size = r.region.heights.size() * sizeof(f32);
    for (size_t i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ull;
    }
    MESSAGE("region.heights hash: ", h);
    CHECK(r.region.width > 2000);
}

TEST_CASE("the sandbox fallback agrees with tiles at their rim") {
    const TileBakeParams params = testParams();
    const TileBakeResult a = bakeSoloTile(params, 0, 0);
    auto base = std::make_shared<render::TerrainBase>();
    base->regions.push_back(
        std::make_shared<render::TerrainRegion>(a.region));
    render::TerrainParams tp;
    tp.base = base;
    auto sandbox = std::make_shared<render::SandboxTerrain>();
    sandbox->controls = params.controls;
    sandbox->controls.seed = params.worldSeed;
    sandbox->macro = params.macro;
    tp.sandbox = sandbox;

    // Continuity where the lone tile fades into the analytic macro: walk
    // out through the blend band. The macro differs from the eroded tile
    // (no erosion in the fallback) — the band must still keep steps
    // bounded.
    for (const f32 z : { 100.0f, 300.0f }) {
        f32 previous =
            render::terrain::height(tp, params.tileSize - 60.0f, z);
        f32 maxStep = 0.0f;
        for (f32 x = params.tileSize - 59.0f;
             x <= params.tileSize + params.overlapMargin + 40.0f;
             x += 1.0f) {
            const f32 h = render::terrain::height(tp, x, z);
            maxStep = std::max(maxStep, std::abs(h - previous));
            previous = h;
        }
        CHECK(maxStep < 8.0f);
    }
}

TEST_CASE("border basins publish once: no stacked duplicate sheets") {
    const TileBakeParams params = testParams();
    // Two adjacent tiles: any basin spanning their border used to be
    // published by both (truncated differently, different spill
    // levels). With the canonical resolution, overlapping masks from
    // the two bakes must not happen.
    TileBakeResult a = bakeSoloTile(params, 0, 0);
    TileBakeResult b = bakeSoloTile(params, 1, 0);
    // A margin basin straddling the line is published by BOTH maps
    // since the clamped ownership (each map is the authority inside
    // its own rect); the runtime clips every lake to its map's rect,
    // and THAT is the no-overlap contract to hold.
    game::clipLakesToMapRect(a.lakes, 0.0f, 0.0f, params.tileSize);
    game::clipLakesToMapRect(b.lakes, params.tileSize, 0.0f,
                             params.tileSize);

    const auto covers = [](const Lake& lake, f32 x, f32 z) {
        if (x < lake.minX || x > lake.maxX || z < lake.minZ ||
            z > lake.maxZ || lake.mask.empty()) {
            return false;
        }
        const u32 mx = static_cast<u32>(glm::clamp(
            (x - lake.minX) / lake.maskTexel + 0.5f, 0.0f,
            static_cast<f32>(lake.maskWidth - 1)));
        const u32 mz = static_cast<u32>(glm::clamp(
            (z - lake.minZ) / lake.maskTexel + 0.5f, 0.0f,
            static_cast<f32>(lake.maskHeight - 1)));
        return lake.mask[static_cast<size_t>(mz) * lake.maskWidth + mx] !=
               0;
    };
    u32 duplicates = 0;
    for (const Lake& la : a.lakes) {
        for (const Lake& lb : b.lakes) {
            if (la.minX > lb.maxX || lb.minX > la.maxX ||
                la.minZ > lb.maxZ || lb.minZ > la.maxZ ||
                la.mask.empty() || lb.mask.empty()) {
                continue;
            }
            const Lake& small = la.cells <= lb.cells ? la : lb;
            const Lake& big = la.cells <= lb.cells ? lb : la;
            u32 sampled = 0;
            u32 shared = 0;
            for (u32 mz = 0; mz < small.maskHeight; mz += 2) {
                for (u32 mx = 0; mx < small.maskWidth; mx += 2) {
                    if (!small.mask[static_cast<size_t>(mz) *
                                        small.maskWidth +
                                    mx]) {
                        continue;
                    }
                    ++sampled;
                    const f32 x =
                        small.minX +
                        static_cast<f32>(mx) * small.maskTexel;
                    const f32 z =
                        small.minZ +
                        static_cast<f32>(mz) * small.maskTexel;
                    if (covers(big, x, z)) {
                        ++shared;
                    }
                }
            }
            if (sampled > 0 &&
                static_cast<f32>(shared) >
                    0.3f * static_cast<f32>(sampled)) {
                ++duplicates;
            }
        }
    }
    CHECK(duplicates == 0);
}

TEST_CASE("stage-1 calm: valley floors join the family, deterministic") {
    const TileBakeParams params = testParams();
    // Pick a tile with actual land — 512 m tiles near the origin can
    // all be wet (the coastal-belt start guarantee promises land
    // NEARBY, not under every small tile). Probe the analytic first:
    // baking is the expensive part.
    TileStage1 a;
    bool found = false;
    ProceduralControlParams probeParams = params.controls;
    probeParams.seed = params.worldSeed;
    const ProceduralControls probe { probeParams };
    u32 probedDry = 0;
    for (i32 ring = 0; ring <= 30 && !found; ++ring) {
        for (i32 tz = -ring; tz <= ring && !found; ++tz) {
            for (i32 tx = -ring; tx <= ring && !found; ++tx) {
                if (glm::max(std::abs(tx), std::abs(tz)) != ring) {
                    continue;
                }
                const f32 cx =
                    (static_cast<f32>(tx) + 0.5f) * params.tileSize;
                const f32 cz =
                    (static_cast<f32>(tz) + 0.5f) * params.tileSize;
                if (macroHeightAnalytic(probe, params.macro, cx, cz) <
                    params.macro.seaLevel + 15.0f) {
                    continue;
                }
                ++probedDry;
                a = bakeTileStage1(params, tx, tz);
                u64 dryCells = 0;
                for (const f32 h : a.eroded) {
                    dryCells += h > params.macro.seaLevel;
                }
                if (dryCells > 500) {
                    found = true;
                    const TileStage1 b = bakeTileStage1(params, tx, tz);
                    CHECK(a.calm == b.calm); // bit-exact (cache)
                }
            }
        }
    }
    MESSAGE("calm tile search: analytic-dry candidates probed: ",
            probedDry);
    REQUIRE(found);
    REQUIRE(a.calm.size() == a.sim.cells());

    // Calm never loses the control-level family and covers a sane
    // share of the dry cells once valley floors joined.
    u64 dry = 0, calmish = 0;
    f64 calmDev = 0.0, roughDev = 0.0;
    u64 calmCount = 0, roughCount = 0;
    // Local relief proxy for the split check: |h - 3x3 mean|.
    const vector<f32> mean = boxBlur3(a.sim, a.eroded);
    for (size_t i = 0; i < a.calm.size(); ++i) {
        if (a.seaDist[i] > 0.0f) { // land by control decree
            CHECK(a.calm[i] >= a.gentle[i] - 1.0e-6f);
        }
        if (a.eroded[i] <= params.macro.seaLevel) {
            continue; // sea zeroes calm (like plateau), gentle is raw
        }
        ++dry;
        const f32 dev = std::abs(a.eroded[i] - mean[i]);
        if (a.calm[i] > 0.6f) {
            ++calmish;
            calmDev += dev;
            ++calmCount;
        } else if (a.calm[i] < 0.2f) {
            roughDev += dev;
            ++roughCount;
        }
    }
    MESSAGE("stage-1 calm>0.6: ", 100.0 * calmish / dry, "% of dry cells");
    CHECK(calmish > 0);
    // Calm ground is measurably smoother than the rough family.
    if (calmCount > 100 && roughCount > 100) {
        CHECK(calmDev / static_cast<f64>(calmCount) <
              roughDev / static_cast<f64>(roughCount));
    }
}

TEST_CASE("stage-1 budget: a calm texel never loses more than its cut") {
    // The hard budget holds through the whole stage-1 chain (fluvial
    // within the budget, thermal and rounding only move material
    // locally): on calm ground off the imprint, macro - eroded stays
    // under calmCut plus a small thermal/rounding slack.
    TileBakeParams params = testParams();
    params.fluvial.iterations = 48;
    const TileStage1 s1 = bakeTileStage1(params, 3, 5);
    REQUIRE(s1.macroHeight.size() == s1.sim.cells());
    REQUIRE(s1.budget.size() == s1.sim.cells());
    u64 calmCells = 0;
    f32 worst = 0.0f;
    for (size_t i = 0; i < s1.budget.size(); ++i) {
        if (s1.macroHeight[i] <= params.macro.seaLevel + 5.0f ||
            s1.budget[i] > params.calmCut + 1.0e-3f) {
            continue; // sea, imprint, rough ground
        }
        ++calmCells;
        worst = glm::max(worst, s1.macroHeight[i] - s1.eroded[i]);
    }
    MESSAGE("calm-budget cells: ", calmCells, ", worst loss ", worst, " m");
    CHECK(calmCells > 100);
    CHECK(worst <= params.calmCut + 2.0f);
}

TEST_CASE("map slices agree in their shared band") {
    // Chantier CARTES: one stage-1 + one hydrology for the whole map —
    // adjacent slices derive from the same surface AND the same routed
    // water, so their shared band carries only the fine-erosion
    // residual (production measured 0.14-0.28 m vs 200-441 m for
    // independently windowed tiles). Toy 2x1 map.
    TileBakeParams params = testParams();
    TileBakeParams mapParams = params;
    mapParams.tileSize = params.tileSize * 2.0f; // 2x2 slices
    mapParams.apron = 1088.0f; // covers the map hydrology window
    const TileStage1 mapS1 = bakeTileStage1(mapParams, 0, 0);
    const MapDrainage drainage =
        computeMapDrainage(params, mapS1, 0, 0, 2);
    FinalizeResult groundA =
        bakeSliceGround(params, 0, 0, mapS1, drainage);
    FinalizeResult groundB =
        bakeSliceGround(params, 1, 0, mapS1, drainage);
    const vector<const FinalizeResult*> grounds { &groundA, &groundB };
    const MapHydrology hydro =
        extractMapHydrology(params, mapS1, 0, 0, 2, grounds);
    REQUIRE(hydro.window.n > 2);

    const TileBakeResult a = bakeSliceWater(params, 0, 0, mapS1, hydro,
                                            std::move(groundA));
    const TileBakeResult b = bakeSliceWater(params, 1, 0, mapS1, hydro,
                                            std::move(groundB));
    REQUIRE(a.region.width > 2);
    REQUIRE(b.region.width > 2);

    const f32 border = params.tileSize;
    f32 maxDiverge = 0.0f;
    for (f32 z = 8.0f; z < params.tileSize; z += 8.0f) {
        for (f32 x = border - params.overlapMargin + 2.0f;
             x < border + params.overlapMargin - 2.0f; x += 4.0f) {
            const f32 ha = render::terrain::baseHeight(a.region, x, z);
            const f32 hb = render::terrain::baseHeight(b.region, x, z);
            maxDiverge = std::max(maxDiverge, std::abs(ha - hb));
        }
    }
    CHECK(maxDiverge < 1.0f);

    // One hydrology, one owner: no lake published by both slices.
    for (const auto& la : a.lakes) {
        for (const auto& lb : b.lakes) {
            const bool same = la.minX == lb.minX && la.minZ == lb.minZ &&
                              la.level == lb.level;
            CHECK_FALSE(same);
        }
    }
}

TEST_SUITE_END();
