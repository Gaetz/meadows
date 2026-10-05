#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>

#include "MapWorldFixture.hpp"
#include "game/MapBaker.hpp"
#include "game/TerrainBakeStreamer.hpp"

// The production path end to end (docs/PAYSAGE.md §7.3, palier B1):
// game::bakeMap on a small bounded map, its cache as the streamer and
// the diagnostics read it, and the content hash that every tuning
// brick re-pins. A 2x2-slice map (8 km, borders on) — the same recipe
// as the 24 km game map, four slices instead of thirty-six.

using namespace render::terraingen;

TEST_SUITE_BEGIN("slow");

namespace {

constexpr i32 kTps = 2;

std::filesystem::path freshCacheRoot() {
    const auto root = std::filesystem::temp_directory_path() /
                      "meadows-mapbake-test" / "1337";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

} // namespace

TEST_CASE("map bake: cache, overview, slices, streamer and content hash") {
    const TileBakeParams params = maptest::gameLikeParams(1337);
    const auto root = freshCacheRoot();
    CHECK_FALSE(game::mapBakedAndValid(root, 0, 0, kTps, &params));

    const game::MapBakeStats stats =
        game::bakeMap(params, 0, 0, root, nullptr, kTps);
    REQUIRE_FALSE(stats.cancelled);
    REQUIRE(stats.slicesWritten == 4);
    MESSAGE("8 km map: stage-1 ", stats.stage1Seconds, " s, slices ",
            stats.sliceSeconds, " s");

    // The manifest is the contract: coords, layout, bake version AND
    // the bake key — a changed data input (sea level, recurve, seed)
    // can never be judged on this cache.
    CHECK(game::mapBakedAndValid(root, 0, 0, kTps));
    CHECK(game::mapBakedAndValid(root, 0, 0, kTps, &params));
    CHECK_FALSE(game::mapBakedAndValid(root, 0, 0, 3)); // another layout
    CHECK_FALSE(game::mapBakedAndValid(root, 1, 0, kTps));
    {
        TileBakeParams other = params;
        other.macro.seaLevel += 1.0f;
        CHECK_FALSE(game::mapBakedAndValid(root, 0, 0, kTps, &other));
        other = params;
        other.macro.recurveMid += 0.05f;
        CHECK_FALSE(game::mapBakedAndValid(root, 0, 0, kTps, &other));
        other = params;
        other.mapGrid.valid = false;
        CHECK_FALSE(game::mapBakedAndValid(root, 0, 0, kTps, &other));
        CHECK(game::mapBakeKey(params, kTps) != game::mapBakeKey(params, 3));
    }

    // The overview: the map stage-1 (map + apron) decimated to 64 m.
    const auto overview = game::loadMapOverview(game::mapCacheDir(root, 0, 0));
    REQUIRE(overview.has_value());
    const f32 mapSize = params.tileSize * static_cast<f32>(kTps);
    CHECK(overview->grid.texelSize == doctest::Approx(64.0f));
    CHECK(overview->grid.originX == doctest::Approx(-kMapApron));
    CHECK(overview->grid.originZ == doctest::Approx(-kMapApron));
    const u32 stage1N =
        static_cast<u32>((mapSize + 2.0f * kMapApron) / params.macroTexel) +
        1;
    CHECK(overview->grid.n == (stage1N - 1) / 4 + 1);
    CHECK(overview->heights.size() == overview->grid.cells());

    // Every slice reads back as the streamer publishes it.
    const maptest::MapWorld world =
        maptest::loadOrBakeMap(params, 0, 0, kTps, root, false);
    REQUIRE(world.slicesLoaded == 4);
    const u32 sliceWidth = static_cast<u32>(
        (params.tileSize + 2.0f * params.overlapMargin) / 2.0f) + 1;
    bool finite = true;
    for (const auto& region : world.tp.base->regions) {
        CHECK(region->width == sliceWidth);
        CHECK(region->height == sliceWidth);
        CHECK(region->texelSize == doctest::Approx(2.0f));
        CHECK(region->maskWidth > 2);
        for (const f32 h : region->heights) {
            finite = finite && std::isfinite(h);
        }
    }
    CHECK(finite);
    CHECK(world.sandbox->overview.size() == overview->heights.size());

    // The published ground and the overview agree to the upsampling
    // error inside the map (the fallback is the map's own truth).
    f32 worstDrift = 0.0f;
    for (f32 z = 1500.0f; z < mapSize - 1500.0f; z += 333.0f) {
        for (f32 x = 1500.0f; x < mapSize - 1500.0f; x += 333.0f) {
            worstDrift = std::max(
                worstDrift,
                std::abs(world.height(x, z) - world.overviewHeight(x, z)));
        }
    }
    MESSAGE("overview vs published ground, worst drift ", worstDrift, " m");
    CHECK(worstDrift < 60.0f);

    // The spawn probe finds temperate land on this map, inside the rim
    // band, on ground that is not under water.
    const auto spawn =
        render::probeMapSpawn(*world.sandbox, 0, 0, params.macro.seaLevel);
    if (spawn) {
        CHECK(world.inside(spawn->x, spawn->z, kMapBorderMountainHalf));
        CHECK(spawn->y > params.macro.seaLevel + 8.0f);
        MESSAGE("spawn probe: (", spawn->x, ", ", spawn->y, ", ", spawn->z,
                ")");
    } else {
        MESSAGE("spawn probe: no temperate candidate on this 8 km map");
    }

    // The streamer, headless: reads the cache, refuses the outside.
    game::TerrainBakeStreamer::MapStreamConfig cfg;
    cfg.tilesPerSide = kTps;
    cfg.mapX = 0;
    cfg.mapZ = 0;
    game::TerrainBakeStreamer streamer { params, root, nullptr, cfg };
    u32 published = 0;
    const auto publish = [&](game::TerrainBakeStreamer::PublishedTile&& tile) {
        CHECK(tile.region.width == sliceWidth);
        CHECK(tile.tx >= 0);
        CHECK(tile.tx < kTps);
        ++published;
    };
    streamer.update({ 0.5f * mapSize, 0.0f, 0.5f * mapSize }, publish);
    CHECK(published == 4);
    CHECK(streamer.publishedCount() == 4);
    CHECK(streamer.pendingCount() == 0);
    streamer.update({ -3.0f * params.tileSize, 0.0f, 0.5f * mapSize },
                    publish);
    CHECK(published == 4); // nothing exists beyond the map rect
    CHECK(streamer.pendingCount() == 0);

    // The content hash: the golden of the bounded map at this
    // kTileBakeVersion. A tuning brick moves it BY DESIGN and re-pins
    // it; anything else that moves it changed the terrain. Pinned per
    // toolchain (float math differs between MSVC and clang — the
    // scatter golden lesson).
    const u64 hash = maptest::heightsHash(world);
    MESSAGE("map heights hash: ", hash);
#if defined(_MSC_VER)
    CHECK(hash == 13401478122622314894ull); // MSVC (Debug == Release), kTileBakeVersion 72
#endif

    // Without its manifest the map is not a map (bake cancelled or
    // interrupted): a partial dir must re-bake, never half-load.
    std::error_code ec;
    std::filesystem::remove(game::mapCacheDir(root, 0, 0) / "manifest.txt",
                            ec);
    CHECK_FALSE(game::mapBakedAndValid(root, 0, 0, kTps, &params));
    std::filesystem::remove_all(root.parent_path(), ec);
}

TEST_SUITE_END();
