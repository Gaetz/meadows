#include <doctest/doctest.h>

#include <chrono>
#include <cmath>

#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/terrain/SandboxTerrain.hpp"
#include "engine/terrain/generation/GridOps.hpp"
#include "engine/terrain/generation/TileBake.hpp"

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

TEST_CASE("tile bakes are deterministic") {
    const TileBakeParams params = testParams();
    const TileBakeResult a = bakeTile(params, 3, -2);
    const TileBakeResult b = bakeTile(params, 3, -2);
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

TEST_CASE("a land tile ships solved water fields on the region rect") {
    const TileBakeParams params = testParams();
    // The test world's origin sits in open ocean (small 512 m tiles):
    // scan the analytic macro outward for the nearest emerged tile, then
    // bake THAT one — no wasted ocean bakes.
    ProceduralControlParams cp = params.controls;
    cp.seed = params.worldSeed;
    const ProceduralControls controls { cp };
    i32 landTx = 0;
    i32 landTz = 0;
    bool foundLand = false;
    for (i32 radius = 0; radius <= 40 && !foundLand; ++radius) {
        for (i32 tz = -radius; tz <= radius && !foundLand; ++tz) {
            for (i32 tx = -radius; tx <= radius && !foundLand; ++tx) {
                if (glm::max(std::abs(tx), std::abs(tz)) != radius) {
                    continue;
                }
                const f32 cx =
                    (static_cast<f32>(tx) + 0.5f) * params.tileSize;
                const f32 cz =
                    (static_cast<f32>(tz) + 0.5f) * params.tileSize;
                if (macroHeightAnalytic(controls, params.macro, cx, cz) >
                    params.macro.seaLevel + 15.0f) {
                    landTx = tx;
                    landTz = tz;
                    foundLand = true;
                }
            }
        }
    }
    REQUIRE(foundLand);
    const render::TerrainRegion region =
        bakeTile(params, landTx, landTz).region;
    f32 maxH = -1.0e9f;
    for (const f32 h : region.heights) {
        maxH = glm::max(maxH, h);
    }
    REQUIRE(maxH > params.macro.seaLevel + 0.5f);
    REQUIRE(region.waterWidth > 2);
    REQUIRE(region.waterDepth.size() ==
            static_cast<size_t>(region.waterWidth) * region.waterHeight);
    CHECK(static_cast<f32>(region.waterWidth - 1) * region.waterTexel ==
          doctest::Approx(region.spanX()));
    // Wet cells carry a level above the local ground (never buried).
    for (u32 row = 0; row < region.waterHeight; ++row) {
        for (u32 col = 0; col < region.waterWidth; ++col) {
            const size_t i =
                static_cast<size_t>(row) * region.waterWidth + col;
            if (region.waterDepth[i] == 0) {
                continue;
            }
            const f32 x = region.originX +
                          static_cast<f32>(col) * region.waterTexel;
            const f32 z = region.originZ +
                          static_cast<f32>(row) * region.waterTexel;
            const f32 ground = render::terrain::baseHeight(region, x, z);
            CHECK(region.waterSurface[i] > ground - 1.0f);
        }
    }
}

TEST_CASE("adjacent tiles blend smoothly across their shared border") {
    const TileBakeParams params = testParams();
    const TileBakeResult a = bakeTile(params, 0, 0);
    const TileBakeResult b = bakeTile(params, 1, 0);

    auto base = std::make_shared<render::TerrainBase>();
    base->regions.push_back(a.region);
    base->regions.push_back(b.region);
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
    for (f32 z = 32.0f; z < params.tileSize; z += 24.0f) {
        for (f32 x = border - params.overlapMargin + 2.0f;
             x < border + params.overlapMargin - 2.0f; x += 8.0f) {
            const f32 ha = render::terrain::baseHeight(a.region, x, z);
            const f32 hb = render::terrain::baseHeight(b.region, x, z);
            maxDiverge = std::max(maxDiverge, std::abs(ha - hb));
        }
    }
    CHECK(maxDiverge < 2.5f);
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

TEST_CASE("stage seam: stage-1 is deterministic, stage-2 composes it") {
    const TileBakeParams params = testParams();
    const TileStage1 s1a = bakeTileStage1(params, 1, 1);
    const TileStage1 s1b = bakeTileStage1(params, 1, 1);
    CHECK(s1a.eroded == s1b.eroded); // bit-exact (disk cache contract)
    CHECK(s1a.seaDist == s1b.seaDist);
    CHECK(s1a.biome == s1b.biome);

    // Center-only stage-2 (every neighbour missing): the contract says
    // the window falls back to the center's sim — valid output, and
    // deterministic.
    const auto centerOnly = [&](i32 qx, i32 qz) -> const TileStage1* {
        return (qx == 1 && qz == 1) ? &s1a : nullptr;
    };
    const TileBakeResult lone = bakeTileStage2(params, 1, 1, centerOnly);
    CHECK(lone.region.width > 2);
    CHECK(lone.region.heights.size() ==
          static_cast<size_t>(lone.region.width) * lone.region.height);
    CHECK(lone.region.originX ==
          doctest::Approx(params.tileSize - params.overlapMargin));
    const TileBakeResult lone2 = bakeTileStage2(params, 1, 1, centerOnly);
    CHECK(lone.region.heights == lone2.region.heights);

    // Full neighbourhood: same call the streamer makes. The kept center
    // heights come from the center stage-1 either way, so the terrain
    // matches the center-only bake away from the water bands.
    TileStage1 grid[3][3];
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            grid[dz + 1][dx + 1] =
                bakeTileStage1(params, 1 + dx, 1 + dz);
        }
    }
    const TileBakeResult full = bakeTileStage2(
        params, 1, 1, [&](i32 qx, i32 qz) -> const TileStage1* {
            const i32 dx = qx - 1;
            const i32 dz = qz - 1;
            if (dx < -1 || dx > 1 || dz < -1 || dz > 1) {
                return nullptr;
            }
            return &grid[dz + 1][dx + 1];
        });
    CHECK(full.region.width == lone.region.width);
    CHECK(full.region.originX == doctest::Approx(lone.region.originX));
}

// Hidden benchmark: full production-size tile (4 km, 1 km apron, 100
// fluvial iterations, 2 m output). Run explicitly with:
//   meadows-tests -tc="*bake benchmark*" -nsf
TEST_CASE("full tile bake benchmark" * doctest::skip()) {
    TileBakeParams params;
    params.worldSeed = 1337;
    const auto start = std::chrono::steady_clock::now();
    const TileBakeResult r = bakeTile(params, 0, 0);
    const f64 seconds =
        std::chrono::duration<f64>(std::chrono::steady_clock::now() -
                                   start)
            .count();
    MESSAGE("full 4 km tile bake: ", seconds, " s, region ",
            r.region.width, "^2 texels, ", r.lakes.size(), " lakes, ",
            r.rivers.size(), " rivers");
    CHECK(r.region.width > 2000);
}

TEST_CASE("the sandbox fallback agrees with tiles at their rim") {
    const TileBakeParams params = testParams();
    const TileBakeResult a = bakeTile(params, 0, 0);
    auto base = std::make_shared<render::TerrainBase>();
    base->regions.push_back(a.region);
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
    const TileBakeResult a = bakeTile(params, 0, 0);
    const TileBakeResult b = bakeTile(params, 1, 0);

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
