#include <doctest/doctest.h>

#include <cmath>

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
    WorldLayerParams p;
    const f32 superSize = p.continentSuperSize;
    REQUIRE(p.continentCellSupers > 0);
    for (const u32 seed : kSeeds) {
        p.seed = seed;
        u32 landSamples = 0;
        // Continent (0,0)'s host core is [0, superSize]^2; the next
        // host is a full pitch away, outside this sweep.
        for (f32 z = -4096.0f; z <= 28672.0f; z += 256.0f) {
            for (f32 x = -4096.0f; x <= 28672.0f; x += 256.0f) {
                const WorldSample w = worldSampleAt(p, x, z);
                if (w.sea && w.base <= 0.0f) {
                    continue;
                }
                ++landSamples;
                const bool inCore = x >= 0.0f && x < superSize &&
                                    z >= 0.0f && z < superSize;
                if (!inCore) {
                    CAPTURE(seed);
                    CAPTURE(x);
                    CAPTURE(z);
                    REQUIRE(inCore);
                }
            }
        }
        // A continent actually exists (the sweep is not all ocean).
        CHECK(landSamples > 1000);
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
    // continents and the pitch lines: no seam bigger than a steep
    // natural gradient over one 16 m step.
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
                CHECK(std::abs(w.base - prevBase) < 12.0f);
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
            CHECK(std::abs(w.base - prevBase) < 12.0f);
        }
        prevBase = w.base;
        first = false;
    }
}
