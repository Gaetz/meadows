#include <doctest/doctest.h>

#include <cmath>

#include "engine/terrain/generation/MapGrid.hpp"
#include "engine/terrain/generation/WorldLayer.hpp"

// The archipelago lattice (docs/PAYSAGE.md §7.8, E1): finite continents
// hosted by one master super-cell each, land kept inside the host core
// by the edge guard — the property every river-head ownership argument
// rests on, so it is asserted here for several seeds.

using namespace render::terraingen;

namespace {

constexpr u32 kSeeds[] = { 1337u, 7u, 42u, 2026u, 99991u };

} // namespace

TEST_CASE("world layer: archipelago land stays inside the host core") {
    // The cores tile the plane (one per super-cell): the ownership
    // invariant is the GUARD — no land within reach of a lattice
    // line, so no continent ever leaks into a neighbour's core and
    // every river head belongs to its window.
    WorldLayerParams p;
    const f32 pitch =
        p.continentSuperSize * static_cast<f32>(p.continentCellSupers);
    const f32 origin = p.continentLatticeOrigin;
    REQUIRE(p.continentCellSupers > 0);
    for (const u32 seed : kSeeds) {
        p.seed = seed;
        for (i32 line = -1; line <= 1; ++line) {
            const f32 at = origin + static_cast<f32>(line) * pitch;
            for (f32 t = origin - pitch; t <= origin + 2.0f * pitch;
                 t += 192.0f) {
                for (const f32 off : { -256.0f, 0.0f, 256.0f }) {
                    const WorldSample a =
                        worldSampleAt(p, at + off, t);
                    const WorldSample b =
                        worldSampleAt(p, t, at + off);
                    CAPTURE(seed);
                    CAPTURE(line);
                    CAPTURE(t);
                    CHECK(a.sea);
                    CHECK(b.sea);
                }
            }
        }
        // And the start continent actually exists.
        u32 landSamples = 0;
        for (f32 z = -8192.0f; z <= 24576.0f; z += 512.0f) {
            for (f32 x = -8192.0f; x <= 24576.0f; x += 512.0f) {
                landSamples += worldSampleAt(p, x, z).sea ? 0 : 1;
            }
        }
        CHECK(landSamples > 500);
    }
}

TEST_CASE("world layer: the start is on land for every seed") {
    WorldLayerParams p;
    for (const u32 seed : kSeeds) {
        p.seed = seed;
        const WorldSample w = worldSampleAt(p, p.startX, p.startZ);
        CAPTURE(seed);
        CHECK_FALSE(w.sea);
        CHECK(w.base > 0.0f);
    }
}

TEST_CASE("world layer: the lattice carrier is continuous") {
    // Across the host-core edge, the deep-ocean midline between two
    // continents and the pitch lines: no SEAM — the fractal coast warp
    // makes real gradients of ~1:1, a true seam would jump the guard
    // depth (~70 m) in one 16 m step.
    WorldLayerParams p;
    const f32 pitch =
        p.continentSuperSize * static_cast<f32>(p.continentCellSupers);
    const auto sweep = [&](bool horizontal, f32 fixed) {
        f32 prevBase = 0.0f;
        f32 prevContinent = 0.0f;
        bool first = true;
        for (f32 t = -8192.0f; t <= pitch + 8192.0f; t += 16.0f) {
            const f32 x = horizontal ? t : fixed;
            const f32 z = horizontal ? fixed : t;
            const WorldSample w = worldSampleAt(p, x, z);
            if (!first) {
                CAPTURE(horizontal);
                CAPTURE(t);
                CHECK(std::abs(w.base - prevBase) < 25.0f);
                CHECK(std::abs(w.continent - prevContinent) < 0.02f);
            }
            prevBase = w.base;
            prevContinent = w.continent;
            first = false;
        }
    };
    sweep(true, 13000.0f);
    sweep(false, 13000.0f);
    // Diagonal through the ocean gap between hosts.
    f32 prevBase = 0.0f;
    bool first = true;
    for (f32 t = 20000.0f; t <= 56000.0f; t += 16.0f) {
        const WorldSample w = worldSampleAt(p, t, t);
        if (!first) {
            CHECK(std::abs(w.base - prevBase) < 25.0f);
        }
        prevBase = w.base;
        first = false;
    }
}

TEST_CASE("map borders: styles read off the world") {
    // Archipelago defaults: an open-ocean line is Sea, the continent's
    // internal lines split between Ridges (massif) and Open (the
    // landscape continues) — the hashed 50/50 is gone. The legacy knob
    // keeps the two-style world.
    WorldLayerParams p;
    const f32 mapSize = 8192.0f;
    // x = 4 * 8192 = 32768: mid-strait between host cores.
    CHECK(mapBorderSegmentStyle(p, mapSize, 4, 0, true) ==
          MapEdgeStyle::Sea);
    u32 open = 0, ridges = 0, seaStyles = 0;
    for (i32 line = 0; line <= 3; ++line) {
        for (i32 cell = 0; cell < 3; ++cell) {
            for (const bool vertical : { true, false }) {
                switch (mapBorderSegmentStyle(p, mapSize, line, cell,
                                              vertical)) {
                case MapEdgeStyle::Open: ++open; break;
                case MapEdgeStyle::Ridges: ++ridges; break;
                default: ++seaStyles; break;
                }
            }
        }
    }
    MESSAGE("continent (0,0) lines: open ", open, ", ridges ", ridges,
            ", sea ", seaStyles);
    CHECK(open >= 1); // « ouvert ailleurs » exists in the start country
    WorldLayerParams legacy;
    legacy.borderFromWorld = false;
    for (i32 line = 0; line <= 3; ++line) {
        for (i32 cell = 0; cell < 3; ++cell) {
            CHECK(mapBorderSegmentStyle(legacy, mapSize, line, cell,
                                        true) != MapEdgeStyle::Open);
        }
    }
}
