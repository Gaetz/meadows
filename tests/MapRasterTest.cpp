#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>

#include "game/MapRaster.hpp"

// The in-game map raster: pure CPU over the pure terrain
// functions, so it doctests headless (the WeaponMeshes precedent:
// meadows-runtime code with no GL in sight).

namespace {

// Flat-world params: zero amplitudes -> height(x, z) == 0 everywhere,
// which makes water/land entirely a matter of seaLevel.
render::TerrainParams flatParams(f32 seaLevel) {
    render::TerrainParams params;
    params.hillAmplitude = 0.0f;
    params.mountainAmplitude = 0.0f;
    params.seaLevel = seaLevel;
    return params;
}

game::MapRasterDesc descFor(const render::TerrainParams& params,
                            u32 size = 16) {
    return { .terrain = &params,
             .minX = -64.0f,
             .minZ = 256.0f,
             .maxX = 192.0f,
             .maxZ = 512.0f,
             .size = size };
}

} // namespace

TEST_CASE("map raster: byte count and determinism") {
    const render::TerrainParams params; // the real defaults, seed 1337
    const game::MapRasterDesc desc = descFor(params, 32);

    const vector<u8> a = game::generateMapRaster(desc);
    CHECK(a.size() == 32u * 32u * 4u);

    // Alpha is opaque everywhere.
    for (size_t i = 3; i < a.size(); i += 4) {
        REQUIRE(a[i] == 255);
    }

    // Bit-identical on a second run (pure functions, no RNG).
    const vector<u8> b = game::generateMapRaster(desc);
    CHECK(a == b);
}

TEST_CASE("map raster: below sea level paints the water family") {
    // Height 0 everywhere, sea at +14: all water. Blue channel dominant.
    const render::TerrainParams params = flatParams(14.0f);
    const vector<u8> pixels = game::generateMapRaster(descFor(params));

    const size_t center = ((8u * 16u) + 8u) * 4u;
    CHECK(pixels[center + 2] > pixels[center + 0]); // b > r
    CHECK(pixels[center + 2] > pixels[center + 1]); // b > g
}

TEST_CASE("map raster: above sea level is land, not water") {
    // Height 0 everywhere, sea far below: all land (flat -> grass).
    const render::TerrainParams params = flatParams(-100.0f);
    const vector<u8> pixels = game::generateMapRaster(descFor(params));

    const size_t center = ((8u * 16u) + 8u) * 4u;
    CHECK(pixels[center + 1] > pixels[center + 2]); // g > b: not water
}

TEST_CASE("map raster: full 512 map size") {
    // The size the game generates (MapController) — real params, the
    // demo overworld extent padded to square.
    const render::TerrainParams params;
    const game::MapRasterDesc desc = { .terrain = &params,
                                       .minX = -64.0f,
                                       .minZ = 224.0f,
                                       .maxX = 256.0f,
                                       .maxZ = 544.0f,
                                       .size = 512 };
    const auto start = std::chrono::steady_clock::now();
    const vector<u8> pixels = game::generateMapRaster(desc);
    const auto ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    std::printf("[bench] 512x512 map raster: %.1f ms\n", ms);
    CHECK(pixels.size() == 512u * 512u * 4u);
}

TEST_CASE("map raster: mapUv corners and center") {
    const render::TerrainParams params;
    const game::MapRasterDesc desc = descFor(params);

    const Vec2 topLeft = game::mapUv(desc, desc.minX, desc.minZ);
    CHECK(topLeft.x == doctest::Approx(0.0f));
    CHECK(topLeft.y == doctest::Approx(0.0f));

    const Vec2 bottomRight = game::mapUv(desc, desc.maxX, desc.maxZ);
    CHECK(bottomRight.x == doctest::Approx(1.0f));
    CHECK(bottomRight.y == doctest::Approx(1.0f));

    const Vec2 center = game::mapUv(desc, (desc.minX + desc.maxX) * 0.5f,
                                    (desc.minZ + desc.maxZ) * 0.5f);
    CHECK(center.x == doctest::Approx(0.5f));
    CHECK(center.y == doctest::Approx(0.5f));

    // Outside the extent clamps (a player past the margin cells stays on
    // the map edge instead of leaving the panel).
    const Vec2 outside = game::mapUv(desc, desc.maxX + 100.0f,
                                     desc.minZ - 100.0f);
    CHECK(outside.x == doctest::Approx(1.0f));
    CHECK(outside.y == doctest::Approx(0.0f));
}
