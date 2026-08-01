#include <doctest/doctest.h>

#include <chrono>
#include <cmath>

#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/terrain/SandboxTerrain.hpp"
#include "engine/terrain/generation/TileBake.hpp"

// Sandbox super-tiles: deterministic bakes, and adjacent tiles that blend
// smoothly through their shared margin ring inside height().

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
