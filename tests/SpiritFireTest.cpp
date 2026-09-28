#include <doctest/doctest.h>

#include <cmath>

#include "world/spirit/SpiritFire.hpp"

// Chantier ESPRITS E3.b: the fire lane's headless half — window, fuel
// blend, the job body.

using namespace world;
using render::terrain::FireCellFuel;

namespace {

const render::terrain::FuelFn grass = [](f32, f32) {
    return FireCellFuel { 3.0f, 1.0f, 0.0f };
};

} // namespace

TEST_CASE("fire lane: the window centers on the focus and scrolls by whole cells") {
    const auto spec = FireWindow::specFor(100.0f, -50.0f);
    CHECK(spec.n == 257);
    CHECK(spec.texelSize == doctest::Approx(2.0f));
    CHECK(spec.originX == doctest::Approx(100.0f - 256.0f));
    CHECK(spec.originZ == doctest::Approx(-50.0f - 256.0f));
    i32 dCol = 99, dRow = 99;
    // Inside the recenter band: no scroll.
    FireWindow::scrollFor(spec, 100.0f + FireWindow::kRecenter * 0.9f, -50.0f,
                          &dCol, &dRow);
    CHECK(dCol == 0);
    CHECK(dRow == 0);
    // Beyond it: a whole-cell shift that recenters.
    FireWindow::scrollFor(spec, 100.0f + 100.0f, -50.0f - 70.0f, &dCol, &dRow);
    CHECK(dCol == 50);
    CHECK(dRow == -35);
}

TEST_CASE("fire lane: fuel blends the splat classes, marsh wetness damps") {
    GroundProps props {};
    props[static_cast<size_t>(GroundClass::Grass)] = { 1.0f, 8.0f, 0.2f, 0.0f, 1.0f };
    props[static_cast<size_t>(GroundClass::Rock)] = { 0.0f, 0.0f, 0.0f, 0.0f, 3.0f };
    const FireCellFuel pure = fuelFromWeights(props, { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f }, 0.0f);
    CHECK(pure.fuel == doctest::Approx(8.0f));
    CHECK(pure.flammability == doctest::Approx(1.0f));
    CHECK(pure.moisture == doctest::Approx(0.2f));
    const FireCellFuel half = fuelFromWeights(props, { 2.0f, 2.0f, 0.0f, 0.0f, 0.0f }, 0.0f);
    CHECK(half.fuel == doctest::Approx(4.0f));
    CHECK(half.flammability == doctest::Approx(0.5f));
    const FireCellFuel marsh = fuelFromWeights(props, { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f }, 0.9f);
    CHECK(marsh.moisture == doctest::Approx(0.9f));
    const FireCellFuel none = fuelFromWeights(props, { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }, 0.0f);
    CHECK(none.fuel == doctest::Approx(0.0f));
}

TEST_CASE("fire lane: a tree heats, catches, burns its canopy, stands bare and regrows") {
    TreeFireParams p;
    p.ignitionSeconds = 4.0f;
    p.burnSeconds = 10.0f;
    p.regrowSeconds = 100.0f;
    TreeFire tree;
    // Half-surrounded: catches after 8 s, not 4.
    bool caught = false;
    f32 t = 0.0f;
    while (!caught && t < 20.0f) {
        caught = advanceTreeFire(tree, 0.5f, 0.1f, p);
        t += 0.1f;
    }
    CHECK(caught);
    CHECK(t == doctest::Approx(8.0f).epsilon(0.05));
    CHECK(tree.phase == TreeFirePhase::Burning);
    // Burning: the canopy goes progressively, whatever the ground does.
    for (int i = 0; i < 50; ++i) {
        advanceTreeFire(tree, 0.0f, 0.1f, p);
    }
    CHECK(tree.burn == doctest::Approx(0.5f).epsilon(0.05));
    for (int i = 0; i < 51; ++i) {
        advanceTreeFire(tree, 0.0f, 0.1f, p);
    }
    CHECK(tree.phase == TreeFirePhase::Burnt);
    CHECK(tree.burn == doctest::Approx(1.0f));
    // Bare for a quarter of the regrowth, then cold and regrowing; fire
    // around a bare tree does nothing.
    for (int i = 0; i < 260; ++i) {
        advanceTreeFire(tree, 1.0f, 0.1f, p);
    }
    CHECK(tree.phase == TreeFirePhase::Cold);
    CHECK(tree.burn < 1.0f);
    CHECK(tree.burn > 0.9f);
    for (int i = 0; i < 1000; ++i) {
        advanceTreeFire(tree, 0.0f, 0.1f, p);
    }
    CHECK(tree.burn == doctest::Approx(0.0f));
    CHECK(tree.idle());
    // Cooling: a tree half-heated and left alone cools down.
    TreeFire cool;
    for (int i = 0; i < 20; ++i) {
        advanceTreeFire(cool, 1.0f, 0.1f, p); // 2 s of 4: heat 0.5
    }
    CHECK(cool.heat == doctest::Approx(0.5f).epsilon(0.05));
    for (int i = 0; i < 100; ++i) {
        advanceTreeFire(cool, 0.0f, 0.1f, p);
    }
    CHECK(cool.heat == doctest::Approx(0.0f));
}

TEST_CASE("fire lane: the job initialises, ignites, steps, masks and scrolls") {
    FireJobInput in;
    in.spec = FireWindow::specFor(0.0f, 0.0f);
    in.params.spreadRate = 4.0f;
    in.params.spreadBudgetPerTick = 1000;
    in.ignitions.push_back({ 0.0f, 0.0f, 1.5f, 2.0f });
    in.steps = 20;
    in.fuel = grass;
    in.epoch = 7;
    FireJobOutput out = runFireJob(std::move(in));
    CHECK(out.epoch == 7);
    CHECK(out.active);
    CHECK(out.stats.burning > 4);
    CHECK(out.grid.valid());
    CHECK(out.scorch.size() == out.grid.cells());
    CHECK(out.glow.size() == out.grid.cells());
    CHECK(!out.burning.empty());
    // The center cell is burning: its scorch grew past zero.
    const u32 n = out.grid.spec.n;
    const size_t center = static_cast<size_t>(n / 2) * n + n / 2;
    CHECK(out.scorch[center] > 0);
    CHECK(out.glow[center] > 100); // burning: it glows
    // Next job: the focus moved east past the band -> scrolled window,
    // the fire still burns in it (world position kept).
    FireJobInput next;
    next.grid = std::move(out.grid);
    next.spec = FireWindow::specFor(100.0f, 0.0f);
    next.params = render::terrain::FireParams {};
    next.params.spreadRate = 4.0f;
    next.params.spreadBudgetPerTick = 1000;
    next.steps = 1;
    next.fuel = grass;
    FireJobOutput scrolled = runFireJob(std::move(next));
    CHECK(scrolled.grid.spec.originX == doctest::Approx(100.0f - 256.0f));
    CHECK(scrolled.stats.burning >= out.stats.burning);
    // The spark cell (world 0, 0) now sits at column 128 - 50 and still
    // burns (its 3 s of fuel are not spent after 2.1 s).
    const size_t moved = static_cast<size_t>(n / 2) * n + (n / 2 - 50);
    CHECK(scrolled.grid.state[moved] ==
          static_cast<u8>(render::terrain::FireState::Burning));
    CHECK(!scrolled.burning.empty());
    // Idle: a window with nothing burning reports inactive.
    FireJobInput idle;
    idle.spec = FireWindow::specFor(0.0f, 0.0f);
    idle.fuel = grass;
    const FireJobOutput quiet = runFireJob(std::move(idle));
    CHECK(!quiet.active);
}
