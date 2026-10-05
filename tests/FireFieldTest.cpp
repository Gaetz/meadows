#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "engine/terrain/FireField.hpp"

// Chantier ESPRITS E3.a: the fire kernel, headless on synthetic fuel.

using namespace render::terrain;

namespace {

render::terraingen::GridSpec spec65() {
    render::terraingen::GridSpec spec;
    spec.originX = 0.0f;
    spec.originZ = 0.0f;
    spec.texelSize = 2.0f;
    spec.n = 65;
    return spec;
}

FireCellFuel grass(f32, f32) { return { 3.0f, 1.0f, 0.0f }; }

FireParams fast() {
    FireParams p;
    p.dt = 0.1f;
    p.spreadRate = 4.0f;      // a neighbour ignites in ~2.5 ticks
    p.ignitionPoints = 1.0f;
    p.burnRate = 0.5f;        // 6 s of flames per cell
    p.spreadBudgetPerTick = 100000;
    return p;
}

// Extent of the burnt+burning region along x and z through the center.
void extent(const FireGrid& g, i32* alongX, i32* alongZ) {
    const i32 n = static_cast<i32>(g.spec.n);
    const auto touched = [&](i32 c, i32 r) {
        const u8 s = g.state[static_cast<size_t>(r) * n + c];
        return s == static_cast<u8>(FireState::Burning) ||
               s == static_cast<u8>(FireState::Burnt);
    };
    i32 xmin = n, xmax = -1, zmin = n, zmax = -1;
    for (i32 r = 0; r < n; ++r) {
        for (i32 c = 0; c < n; ++c) {
            if (touched(c, r)) {
                xmin = std::min(xmin, c); xmax = std::max(xmax, c);
                zmin = std::min(zmin, r); zmax = std::max(zmax, r);
            }
        }
    }
    *alongX = xmax - xmin + 1;
    *alongZ = zmax - zmin + 1;
}

} // namespace

TEST_CASE("fire: a spark on grass spreads as a disc without wind") {
    FireGrid g;
    fireInitWindow(g, spec65());
    fireIgnite(g, 64.0f, 64.0f, 1.0f, 2.0f, grass);
    const FireParams p = fast();
    FireStats stats;
    for (int t = 0; t < 40; ++t) {
        fireStep(g, p, grass, nullptr, &stats);
    }
    CHECK(stats.burning > 8);
    i32 ax = 0, az = 0;
    extent(g, &ax, &az);
    CHECK(ax == az); // round
    CHECK(ax > 5);
    CHECK(ax < 60);  // and bounded: the front walks, it does not teleport
}

TEST_CASE("fire: wind stretches the front downwind") {
    FireGrid g;
    fireInitWindow(g, spec65());
    fireIgnite(g, 64.0f, 64.0f, 1.0f, 2.0f, grass);
    FireParams p = fast();
    p.wind = { 1.0f, 0.0f }; // blowing +x
    for (int t = 0; t < 40; ++t) {
        fireStep(g, p, grass, nullptr);
    }
    // The front is a cone opening downwind (the diagonal chains widen
    // it as it advances), so the bounding box stays about square...
    i32 ax = 0, az = 0;
    extent(g, &ax, &az);
    CHECK(static_cast<f32>(ax) / static_cast<f32>(az) > 0.8f);
    // ...but the burnt region reaches FAR further east than west of the
    // spark: that is the bell-shaped windward front.
    const i32 n = 65;
    i32 east = 0, west = 0;
    for (i32 c = 32; c < n; ++c) {
        if (g.state[32 * n + c] != 0) east = c - 32;
    }
    for (i32 c = 32; c >= 0; --c) {
        if (g.state[32 * n + c] != 0) west = 32 - c;
    }
    CHECK(east > 2 * west);
}

TEST_CASE("fire: a gust bends the front where it blows, the calm side spreads as ever") {
    FireGrid g;
    fireInitWindow(g, spec65());
    fireIgnite(g, 64.0f, 64.0f, 1.0f, 2.0f, grass);
    const FireParams p = fast(); // no uniform wind
    // A gust blowing +x over the spark's neighbourhood only.
    const WindFn gust = [](f32 x, f32 z) {
        const f32 dx = x - 64.0f, dz = z - 64.0f;
        return dx * dx + dz * dz < 30.0f * 30.0f ? Vec2 { 1.0f, 0.0f } : Vec2 { 0.0f };
    };
    for (int t = 0; t < 40; ++t) {
        fireStep(g, p, grass, nullptr, nullptr, gust);
    }
    const i32 n = 65;
    i32 east = 0, west = 0;
    for (i32 c = 32; c < n; ++c) {
        if (g.state[32 * n + c] != 0) east = c - 32;
    }
    for (i32 c = 32; c >= 0; --c) {
        if (g.state[32 * n + c] != 0) west = 32 - c;
    }
    CHECK(east > 2 * west);
}

TEST_CASE("fire: a bare rock band stops the front, wet ground never catches") {
    // Rock (no fuel) for x in [80, 90): the fire never crosses it.
    const FuelFn banded = [](f32 x, f32) -> FireCellFuel {
        if (x >= 80.0f && x < 90.0f) return { 0.0f, 0.0f, 0.0f };
        return { 3.0f, 1.0f, 0.0f };
    };
    FireGrid g;
    fireInitWindow(g, spec65());
    fireIgnite(g, 64.0f, 64.0f, 1.0f, 2.0f, banded);
    const FireParams p = fast();
    for (int t = 0; t < 120; ++t) {
        fireStep(g, p, banded, nullptr);
    }
    const i32 n = 65;
    for (i32 r = 0; r < n; ++r) {
        for (i32 c = 46; c < n; ++c) { // x >= 92: beyond the band
            CHECK(g.state[r * n + c] == static_cast<u8>(FireState::Dormant));
        }
    }
    // Soaked fuel: moisture 1 takes no heat at all.
    const FuelFn soaked = [](f32, f32) -> FireCellFuel { return { 3.0f, 1.0f, 1.0f }; };
    FireGrid wetGrass;
    fireInitWindow(wetGrass, spec65());
    fireIgnite(wetGrass, 64.0f, 64.0f, 1.0f, 2.0f, soaked);
    FireStats stats;
    for (int t = 0; t < 40; ++t) {
        fireStep(wetGrass, p, soaked, nullptr, &stats);
    }
    CHECK(stats.burning <= 1); // the spark cell alone (ignited by the source)
}

TEST_CASE("fire: standing water douses a burning cell in one tick and keeps it out") {
    FireGrid g;
    fireInitWindow(g, spec65());
    fireIgnite(g, 64.0f, 64.0f, 3.0f, 2.0f, grass);
    const FireParams p = fast();
    FireStats stats;
    fireStep(g, p, grass, nullptr, &stats);
    CHECK(stats.burning > 1);
    // A pond appears under everything: all doused, nothing burns.
    const WetFn pond = [](f32, f32) { return true; };
    fireStep(g, p, grass, pond, &stats);
    CHECK(stats.doused > 0);
    CHECK(stats.burning == 0);
    for (int t = 0; t < 10; ++t) {
        fireStep(g, p, grass, pond, &stats);
    }
    CHECK(stats.burning == 0);
    // The pond dries: cells are dormant again (not burnt — they were
    // put out with fuel left).
    const WetFn dry = [](f32, f32) { return false; };
    fireStep(g, p, grass, dry, &stats);
    const size_t center = 32 * 65 + 32;
    CHECK(g.state[center] == static_cast<u8>(FireState::Dormant));
    CHECK(g.fuel[center] > 0.0f);
}

TEST_CASE("fire: burnt ground regrows, the wetter the sooner, and can burn again") {
    // Two cells: dry grass and marsh grass (moisture 0.75 = 3.25x faster).
    const FuelFn banded = [](f32 x, f32) -> FireCellFuel {
        return x < 64.0f ? FireCellFuel { 1.0f, 1.0f, 0.0f }
                         : FireCellFuel { 1.0f, 1.0f, 0.75f };
    };
    FireGrid g;
    fireInitWindow(g, spec65());
    FireParams p = fast();
    p.regrowSeconds = 4.0f; // dry: 40 ticks; marsh: ~12 ticks
    p.spreadRate = 0.0f;    // no spread: the two sparks stay put
    fireIgnite(g, 62.0f, 64.0f, 0.5f, 2.0f, banded);
    fireIgnite(g, 66.0f, 64.0f, 0.5f, 2.0f, banded);
    const size_t dry = 32 * 65 + 31;
    const size_t wet = 32 * 65 + 33;
    // 1 s of fuel at 0.5/s = 20 ticks to burn out.
    for (int t = 0; t < 21; ++t) {
        fireStep(g, p, banded, nullptr);
    }
    CHECK(g.state[dry] == static_cast<u8>(FireState::Burnt));
    CHECK(g.state[wet] == static_cast<u8>(FireState::Burnt));
    for (int t = 0; t < 20; ++t) {
        fireStep(g, p, banded, nullptr);
    }
    CHECK(g.state[wet] == static_cast<u8>(FireState::Dormant)); // marsh: back
    CHECK(g.state[dry] == static_cast<u8>(FireState::Burnt));   // dry: not yet
    vector<u8> scorch;
    fireScorch(g, p, scorch);
    CHECK(scorch[wet] == 0);
    CHECK(scorch[dry] > 0);
    CHECK(scorch[dry] < 255); // fading as it regrows
    for (int t = 0; t < 25; ++t) {
        fireStep(g, p, banded, nullptr);
    }
    CHECK(g.state[dry] == static_cast<u8>(FireState::Dormant));
    CHECK(g.fuel[dry] == doctest::Approx(1.0f)); // fuel restored
    // It burns again.
    fireIgnite(g, 62.0f, 64.0f, 0.5f, 2.0f, banded);
    fireStep(g, p, banded, nullptr);
    CHECK(g.state[dry] == static_cast<u8>(FireState::Burning));
}

TEST_CASE("fire: a douse puts the cells out, keeps their fuel, and they catch again") {
    FireGrid g;
    fireInitWindow(g, spec65());
    const FireParams p = fast();
    fireIgnite(g, 64.0f, 64.0f, 3.0f, 2.0f, grass);
    FireStats stats;
    fireStep(g, p, grass, nullptr, &stats);
    CHECK(stats.burning > 1);
    fireDouse(g, 64.0f, 64.0f, 6.0f);
    fireStep(g, p, grass, nullptr, &stats);
    CHECK(stats.burning == 0);
    const size_t center = 32 * 65 + 32;
    CHECK(g.state[center] == static_cast<u8>(FireState::Dormant));
    CHECK(g.fuel[center] > 2.0f); // barely burnt: the fuel stays
    fireIgnite(g, 64.0f, 64.0f, 1.0f, 2.0f, grass);
    fireStep(g, p, grass, nullptr, &stats);
    CHECK(stats.burning >= 1);
}

TEST_CASE("fire: the ignition budget caps how many cells catch per tick") {
    FireGrid g;
    fireInitWindow(g, spec65());
    fireIgnite(g, 64.0f, 64.0f, 20.0f, 5.0f, grass); // a wide blast: everything hot
    FireParams p = fast();
    p.spreadBudgetPerTick = 7;
    FireStats stats;
    fireStep(g, p, grass, nullptr, &stats);
    CHECK(stats.ignited == 7);
    CHECK(stats.burning == 7);
    fireStep(g, p, grass, nullptr, &stats);
    CHECK(stats.ignited == 7); // the rest waits its turn, heat kept
    CHECK(stats.burning == 14);
}

TEST_CASE("fire: burning consumes the fuel into burnt ground, scorch reads it, two runs are bit-exact") {
    FireGrid a, b;
    fireInitWindow(a, spec65());
    fireInitWindow(b, spec65());
    fireIgnite(a, 64.0f, 64.0f, 1.0f, 2.0f, grass);
    fireIgnite(b, 64.0f, 64.0f, 1.0f, 2.0f, grass);
    FireParams p = fast();
    p.wind = { 0.3f, 0.7f };
    for (int t = 0; t < 80; ++t) {
        fireStep(a, p, grass, nullptr);
        fireStep(b, p, grass, nullptr);
    }
    CHECK(a.heat == b.heat);
    CHECK(a.fuel == b.fuel);
    CHECK(a.state == b.state);
    // The spark cell burnt through its 3 s of fuel (6 s at 0.5/s = 60 ticks).
    const size_t center = 32 * 65 + 32;
    CHECK(a.state[center] == static_cast<u8>(FireState::Burnt));
    vector<u8> scorch;
    fireScorch(a, p, scorch);
    CHECK(scorch[center] > 240); // burnt 2 s ago: barely regrown
    CHECK(scorch[0] == 0);
    // The ember mask: the front burning bright, the burnt center still
    // glowing with its cooling embers (2 s into 45), cold ground 0.
    vector<u8> glow;
    fireGlow(a, p, glow);
    CHECK(glow[center] > 100);
    CHECK(glow[center] < 150);
    CHECK(glow[0] == 0);
    u8 brightest = 0;
    for (const u8 g : glow) {
        brightest = std::max(brightest, g);
    }
    CHECK(brightest > 140); // 0.55 + fresh fuel
    // Embers cool over emberSeconds: with 1 s they are out at 2 s.
    FireGrid fast;
    fireInitWindow(fast, spec65());
    fireIgnite(fast, 64.0f, 64.0f, 1.0f, 2.0f, grass);
    p.emberSeconds = 1.0f;
    for (int t = 0; t < 80; ++t) {
        fireStep(fast, p, grass, nullptr);
    }
    fireGlow(fast, p, glow);
    CHECK(glow[center] == 0);
    // Burning cells report as the emitter budget, freshest first.
    const vector<Vec2> centers = fireBurningCenters(a, 5);
    CHECK(centers.size() == 5);
    // The front: only the cells ignited within the last second.
    const vector<Vec2> front = fireBurningCenters(a, 1000, p, 1.0f);
    const vector<Vec2> all = fireBurningCenters(a, 1000);
    CHECK(!front.empty());
    CHECK(front.size() < all.size());
    // Scrolling keeps the interior bit-exact at its new index.
    FireGrid c = a;
    fireScrollWindow(c, 3, -2);
    const i32 n = 65;
    // New (row, col) reads old (row + dRow, col + dCol).
    for (i32 r = 2; r < n; ++r) {
        for (i32 col = 0; col < n - 3; ++col) {
            CHECK(c.state[r * n + col] == a.state[(r - 2) * n + col + 3]);
        }
    }
    CHECK(c.spec.originX == doctest::Approx(6.0f));
    CHECK(c.spec.originZ == doctest::Approx(-4.0f));
}

TEST_CASE("fire kernel: the rain damps the spread and soaks the flames out") {
    FireGrid dry;
    fireInitWindow(dry, spec65());
    fireIgnite(dry, 64.0f, 64.0f, 1.0f, 2.0f, grass);
    FireGrid wet = dry;
    FireParams p = fast();
    FireParams rainy = p;
    rainy.rain = 1.0f;
    FireStats dryStats, wetStats;
    for (int i = 0; i < 40; ++i) {
        fireStep(dry, p, grass, {}, &dryStats);
        fireStep(wet, rainy, grass, {}, &wetStats);
    }
    // Full rain: nothing caught beyond the spark (4 s stepped, under the
    // 6 s soak: the spark still burns but spread nothing).
    CHECK(dryStats.burning > wetStats.burning);
    CHECK(wetStats.ignited == 0);
    for (int i = 0; i < 30; ++i) {
        fireStep(wet, rainy, grass, {}, &wetStats);
    }
    CHECK(wetStats.burning == 0); // 7 s of rain: soaked out
    // A light drizzle only slows the front.
    FireGrid drizzle;
    fireInitWindow(drizzle, spec65());
    fireIgnite(drizzle, 64.0f, 64.0f, 1.0f, 2.0f, grass);
    FireParams light = p;
    light.rain = 0.3f;
    FireStats drizzleStats;
    for (int i = 0; i < 40; ++i) {
        fireStep(drizzle, light, grass, {}, &drizzleStats);
    }
    CHECK(drizzleStats.burning > 0);
    CHECK(drizzleStats.burning < dryStats.burning);
}

TEST_CASE("fire kernel: saved cells restore into a fresh window") {
    FireGrid grid;
    fireInitWindow(grid, spec65());
    fireIgnite(grid, 64.0f, 64.0f, 1.0f, 2.0f, grass);
    const FireParams p = fast();
    for (int i = 0; i < 30; ++i) {
        fireStep(grid, p, grass, {});
    }
    vector<FireSavedCell> cells;
    fireCollectCells(grid, cells);
    CHECK(!cells.empty());
    u32 burning = 0;
    for (const FireSavedCell& c : cells) {
        CHECK((c.state == FireState::Burning || c.state == FireState::Burnt));
        if (c.state == FireState::Burning) {
            ++burning;
        }
    }
    CHECK(burning > 0);
    FireGrid fresh;
    fireInitWindow(fresh, spec65());
    for (const FireSavedCell& c : cells) {
        fireRestoreCell(fresh, c.x, c.z, c.state, c.fuelFraction, c.regrow,
                        c.ember, grass);
    }
    vector<FireSavedCell> again;
    fireCollectCells(fresh, again);
    REQUIRE(again.size() == cells.size());
    for (size_t i = 0; i < cells.size(); ++i) {
        CHECK(again[i].x == doctest::Approx(cells[i].x));
        CHECK(again[i].state == cells[i].state);
        CHECK(again[i].fuelFraction == doctest::Approx(cells[i].fuelFraction).epsilon(0.01));
    }
    // A cell outside the window is ignored, not written anywhere.
    fireRestoreCell(fresh, 5000.0f, 5000.0f, FireState::Burning, 1.0f, 0.0f, 0.0f, grass);
    vector<FireSavedCell> same;
    fireCollectCells(fresh, same);
    CHECK(same.size() == again.size());
    // And the restored fire keeps burning.
    FireStats stats;
    fireStep(fresh, p, grass, {}, &stats);
    CHECK(stats.burning >= burning); // it burns on (and spreads: heat was not saved)
}

TEST_CASE("fire kernel: calm air creeps, the wind drives the front downwind") {
    const FireParams calm = fast();
    FireParams windy = fast();
    windy.wind = { 1.0f, 0.0f }; // a full wind blowing +x
    FireGrid a, b;
    fireInitWindow(a, spec65());
    fireInitWindow(b, spec65());
    fireIgnite(a, 64.0f, 64.0f, 1.0f, 2.0f, grass);
    fireIgnite(b, 64.0f, 64.0f, 1.0f, 2.0f, grass);
    for (int t = 0; t < 40; ++t) {
        fireStep(a, calm, grass, nullptr);
        fireStep(b, windy, grass, nullptr);
    }
    const i32 n = 65;
    const auto reach = [&](const FireGrid& g, i32 step) {
        i32 r = 0;
        for (i32 c = 32; c >= 0 && c < n; c += step) {
            if (g.state[32 * n + c] != 0) r = std::abs(c - 32);
        }
        return r;
    };
    const i32 calmEast = reach(a, 1);
    const i32 windEast = reach(b, 1);
    const i32 windWest = reach(b, -1);
    CHECK(calmEast > 2);                 // it does creep
    CHECK(windEast >= 2 * calmEast);     // the wind drives it
    CHECK(windWest <= calmEast);         // and holds the upwind side back
    CHECK(windEast > 3 * windWest);
}

TEST_CASE("fire kernel: a driven front never climbs the wind (shipped tuning)") {
    // The base game's figures: a trickle upwind stays under the decay.
    FireParams p;
    p.dt = 0.1f;
    p.spreadRate = 1.0f;
    p.ignitionPoints = 1.0f;
    p.heatDecay = 0.35f;
    p.burnRate = 1.0f / 8.0f;
    p.spreadBudgetPerTick = 100000;
    p.wind = { 1.0f, 0.0f };
    FireGrid g;
    fireInitWindow(g, spec65());
    fireIgnite(g, 64.0f, 64.0f, 2.5f, 2.0f, grass); // the spell's disc
    fireStep(g, p, grass, nullptr); // the disc itself catches
    i32 west0 = 0;
    const i32 n = 65;
    for (i32 c = 32; c >= 0; --c) {
        if (g.state[32 * n + c] != 0) west0 = 32 - c;
    }
    for (int t = 0; t < 150; ++t) { // 15 s
        fireStep(g, p, grass, nullptr);
    }
    i32 east = 0, west = 0;
    for (i32 c = 32; c < n; ++c) {
        if (g.state[32 * n + c] != 0) east = c - 32;
    }
    for (i32 c = 32; c >= 0; --c) {
        if (g.state[32 * n + c] != 0) west = 32 - c;
    }
    CHECK(west == west0);  // not one cell gained against the wind
    CHECK(east > west0 + 4); // while it ran downwind
    // And the same tuning still creeps in calm air.
    FireParams calm = p;
    calm.wind = { 0.0f, 0.0f };
    FireGrid c;
    fireInitWindow(c, spec65());
    fireIgnite(c, 64.0f, 64.0f, 2.5f, 2.0f, grass);
    for (int t = 0; t < 150; ++t) {
        fireStep(c, calm, grass, nullptr);
    }
    i32 calmEast = 0;
    for (i32 k = 32; k < n; ++k) {
        if (c.state[32 * n + k] != 0) calmEast = k - 32;
    }
    CHECK(calmEast > west0 + 1);
}
