#include <doctest/doctest.h>

#include <chrono>
#include <cmath>

#include "engine/terrain/WaterSim.hpp"

// The real-time windowed water sim (option C): kernel physics, scroll
// mechanics, determinism, and the perf premise — all headless on
// synthetic HeightFns. The offline solveSteadyWater is the equilibrium
// oracle (the kernel exists twice on purpose; the cross-check below is
// what allows that).

using namespace render::terrain;
using render::terraingen::GridSpec;
using render::terraingen::WaterSource;

namespace {

GridSpec makeSpec(u32 n, f32 texel, f32 originX = 0.0f,
                  f32 originZ = 0.0f) {
    GridSpec spec;
    spec.originX = originX;
    spec.originZ = originZ;
    spec.texelSize = texel;
    spec.n = n;
    return spec;
}

f64 totalVolume(const WaterSimState& state) {
    f64 sum = 0.0;
    for (const f32 d : state.depth) {
        sum += d;
    }
    return sum * state.spec.texelSize * state.spec.texelSize;
}

// A closed bowl well above sea level: nothing drains, nothing pins.
constexpr f32 kBowlFloor = 500.0f;
f32 bowlHeight(f32 x, f32 z) {
    const f32 dx = (x - 64.0f) / 64.0f;
    const f32 dz = (z - 64.0f) / 64.0f;
    return kBowlFloor + 30.0f * (dx * dx + dz * dz);
}

WaterSimParams closedParams() {
    WaterSimParams params;
    params.rainRate = 0.0f;
    params.evaporationRate = 0.0f;
    params.borderDrainPerSecond = 0.0f;
    params.seaLevel = -1000.0f; // no sea anywhere
    return params;
}

} // namespace

TEST_CASE("water sim: closed volume is conserved") {
    const GridSpec spec = makeSpec(65, 2.0f);
    WaterSimState state;
    initWindow(state, spec, bowlHeight, -1000.0f);
    // Drop a column in the bowl (the flood start is dry: bowl drains
    // nowhere but priority-flood epsilon keeps it empty of rain).
    const size_t center = 32u * spec.n + 32u;
    state.depth[center] = 4.0f;
    const WaterSimParams params = closedParams();
    const f64 before = totalVolume(state);
    stepWindow(state, params, {}, 5000);
    const f64 after = totalVolume(state);
    CHECK(std::abs(after - before) / before < 1.0e-4);
    for (const f32 d : state.depth) {
        CHECK(std::isfinite(d));
    }
}

TEST_CASE("water sim: sources add exactly their discharge") {
    const GridSpec spec = makeSpec(65, 2.0f);
    WaterSimState state;
    initWindow(state, spec, bowlHeight, -1000.0f);
    const WaterSimParams params = closedParams();
    const vector<WaterSource> sources { { 64.0f, 64.0f, 0.5f } };
    const f64 before = totalVolume(state);
    const u32 steps = 600;
    stepWindow(state, params, sources, steps);
    const f64 added = totalVolume(state) - before;
    const f64 expected =
        0.5 * static_cast<f64>(params.dt) * static_cast<f64>(steps);
    CHECK(added == doctest::Approx(expected).epsilon(0.001));
}

TEST_CASE("water sim: dam break spreads symmetrically") {
    const GridSpec spec = makeSpec(65, 2.0f);
    WaterSimState state;
    initWindow(
        state, spec, [](f32, f32) { return kBowlFloor; }, -1000.0f);
    const size_t center = 32u * spec.n + 32u;
    state.depth[center] = 6.0f;
    stepWindow(state, closedParams(), {}, 200);
    // Mirror symmetry in x and z (a strong kernel-typo detector), and
    // the column actually spread.
    CHECK(state.depth[center] < 6.0f);
    u32 wet = 0;
    for (u32 row = 0; row < spec.n; ++row) {
        for (u32 col = 0; col <= 32; ++col) {
            const size_t i = static_cast<size_t>(row) * spec.n + col;
            const size_t mx =
                static_cast<size_t>(row) * spec.n + (64 - col);
            CHECK(state.depth[i] == state.depth[mx]); // bit-exact
            if (state.depth[i] > 0.001f) {
                ++wet;
            }
        }
    }
    CHECK(wet > 20);
    // Z mirror is NOT bit-exact by construction: the divergence sum
    // (fE + fW + fS + fN) keeps its order under the mirror while fS/fN
    // swap values — float associativity leaves last-ulp residue. The
    // X mirror IS bit-exact (fE/fW swap commutes). Epsilon here.
    for (u32 col = 0; col < spec.n; ++col) {
        for (u32 row = 0; row <= 32; ++row) {
            const size_t i = static_cast<size_t>(row) * spec.n + col;
            const size_t mz =
                static_cast<size_t>(64 - row) * spec.n + col;
            CHECK(std::abs(state.depth[i] - state.depth[mz]) < 1.0e-5f);
        }
    }
}

TEST_CASE("water sim: a tilted channel carries its discharge") {
    // V-channel sloping +x; source upstream; after settling, the
    // eastward through-flux at several cross-sections matches the
    // injected discharge.
    const GridSpec spec = makeSpec(97, 2.0f);
    const auto channel = [](f32 x, f32 z) {
        const f32 lateral = std::abs(z - 96.0f) * 0.5f;
        return 300.0f - x * 0.02f + lateral;
    };
    WaterSimState state;
    initWindow(state, spec, channel, -1000.0f);
    WaterSimParams params = closedParams();
    params.borderDrainPerSecond = 0.9f; // outflow at the downhill edge
    const vector<WaterSource> sources { { 8.0f, 96.0f, 0.8f } };
    stepWindow(state, params, sources, 6000);
    for (const u32 col : { 24u, 48u, 72u }) {
        f64 flux = 0.0;
        for (u32 row = 0; row < spec.n; ++row) {
            const size_t i = static_cast<size_t>(row) * spec.n + col;
            flux += state.fE[i] - state.fW[i + 1];
        }
        CHECK(flux == doctest::Approx(0.8).epsilon(0.05));
    }
}

TEST_CASE("water sim: a waterfall column descends a cliff step") {
    // Upper flat -> 20 m cliff -> lower flat; source on the upper
    // flat. Water must reach the lower flat, stay finite, and respect
    // the velocity cap at extraction.
    const GridSpec spec = makeSpec(65, 2.0f);
    const auto cliff = [](f32 x, f32) {
        return x < 64.0f ? 320.0f : 300.0f;
    };
    WaterSimState state;
    initWindow(state, spec, cliff, -1000.0f);
    WaterSimParams params = closedParams();
    params.borderDrainPerSecond = 0.9f;
    const vector<WaterSource> sources { { 16.0f, 64.0f, 0.6f } };
    stepWindow(state, params, sources, 4000);
    f32 lowerWet = 0.0f;
    for (u32 row = 0; row < spec.n; ++row) {
        for (u32 col = 40; col < spec.n; ++col) {
            lowerWet += state.depth[static_cast<size_t>(row) * spec.n +
                                    col];
        }
    }
    CHECK(lowerWet > 0.01f);
    WaterSimSnapshot snap;
    extractSnapshot(state, params, snap);
    for (size_t i = 0; i < snap.depth.size(); ++i) {
        CHECK(std::isfinite(snap.depth[i]));
        CHECK(std::hypot(snap.velX[i], snap.velZ[i]) <= 12.01f);
    }
}

TEST_CASE("water sim: scroll shifts the interior bit-exactly") {
    const GridSpec spec = makeSpec(65, 2.0f);
    WaterSimState state;
    initWindow(state, spec, bowlHeight, -1000.0f);
    const size_t center = 32u * spec.n + 32u;
    state.depth[center] = 3.0f;
    stepWindow(state, closedParams(), {}, 50);
    const WaterSimState before = state;
    scrollWindow(state, 5, -3, bowlHeight, -1000.0f);
    CHECK(state.spec.originX ==
          doctest::Approx(before.spec.originX + 10.0f));
    CHECK(state.spec.originZ ==
          doctest::Approx(before.spec.originZ - 6.0f));
    // Overlap: the origin moved +5 cols / -3 rows, so new (col,row)
    // shows old (col+5, row-3). Interior away from the entered strips
    // (high cols, low rows) AND the re-zeroed walls must match.
    for (u32 row = 4; row < 60; ++row) {
        for (u32 col = 1; col < 55; ++col) {
            const size_t i = static_cast<size_t>(row) * spec.n + col;
            const size_t j =
                static_cast<size_t>(row - 3) * spec.n + (col + 5);
            CHECK(state.depth[i] == before.depth[j]);
            CHECK(state.terrain[i] == before.terrain[j]);
            CHECK(state.fE[i] == before.fE[j]);
        }
    }
}

TEST_CASE("water sim: scroll strips enter dry — no invented water") {
    // The old strip init swept the surviving edge's surface across the
    // whole strip: flying past a lake painted its level over entered
    // valleys (replay-measured flood). Strips must enter DRY; lakes
    // re-arrive through pinLakes, rivers through the sources.
    const GridSpec spec = makeSpec(65, 2.0f);
    WaterSimState state;
    initWindow(state, spec, bowlHeight, -1000.0f);
    // Deep water column sitting right at the future survivor edge.
    for (u32 row = 20; row < 45; ++row) {
        state.depth[static_cast<size_t>(row) * spec.n + 58u] = 30.0f;
    }
    scrollWindow(state, 6, 0, bowlHeight, -1000.0f);
    for (u32 row = 0; row < spec.n; ++row) {
        for (u32 col = 59; col < spec.n; ++col) {
            CHECK(state.depth[static_cast<size_t>(row) * spec.n + col] ==
                  0.0f);
        }
    }
}

TEST_CASE("water sim: command list is bit-deterministic") {
    const auto run = [] {
        const GridSpec spec = makeSpec(65, 2.0f);
        WaterSimState state;
        initWindow(state, spec, bowlHeight, -1000.0f);
        WaterSimParams params = closedParams();
        params.rainRate = 2.0e-5f;
        const vector<WaterSource> sources { { 40.0f, 40.0f, 0.2f } };
        stepWindow(state, params, sources, 120);
        scrollWindow(state, 4, 2, bowlHeight, -1000.0f);
        stepWindow(state, params, sources, 120);
        return state;
    };
    const WaterSimState a = run();
    const WaterSimState b = run();
    CHECK(a.depth == b.depth);
    CHECK(a.fE == b.fE);
    CHECK(a.fN == b.fN);
}

TEST_CASE("water sim: matches the offline equilibrium oracle") {
    // The kernel exists twice (offline solver + real-time stepper);
    // this cross-check is the guard against silent divergence. Tilted
    // valley with a sea edge, rain on — both must settle to the same
    // surface.
    const GridSpec spec = makeSpec(65, 4.0f);
    const auto valley = [](f32 x, f32 z) {
        const f32 lateral = std::abs(z - 128.0f) * 0.12f;
        f32 h = 18.0f + x * 0.05f + lateral; // west edge under sea 21
        // A basin mid-valley: both solvers must fill it to the same
        // spill level — the non-trivial part of the comparison.
        const f32 d2 = (x - 160.0f) * (x - 160.0f) +
                       (z - 128.0f) * (z - 128.0f);
        if (d2 < 1600.0f) {
            h -= 2.0f * (1.0f - d2 / 1600.0f);
        }
        return h;
    };
    vector<f32> terrain(spec.cells());
    for (u32 row = 0; row < spec.n; ++row) {
        for (u32 col = 0; col < spec.n; ++col) {
            terrain[static_cast<size_t>(row) * spec.n + col] =
                valley(spec.x(col), spec.z(row));
        }
    }
    WaterSimParams params;
    params.dt = 0.05f * std::sqrt(4.0f / 2.0f);
    params.rainRate = 3.0e-4f;
    params.evaporationRate = 2.0e-5f;
    params.seaLevel = 21.0f;
    params.borderDrainPerSecond = 0.4f;

    render::terraingen::WaterSolveParams solve;
    solve.dt = params.dt;
    solve.rainRate = params.rainRate;
    solve.evaporationRate = params.evaporationRate;
    solve.seaLevel = params.seaLevel;
    solve.dryThreshold = 0.0f;
    solve.warmStart = false; // both sides from dry (no-phantom doctrine)
    solve.multigrid = false;
    solve.maxIterations = 12000;
    const auto oracle = render::terraingen::solveSteadyWater(
        spec, terrain, solve, nullptr);

    WaterSimState state;
    initWindow(state, spec, valley, params.seaLevel);
    stepWindow(state, params, {}, 12000);

    // Compare surfaces on cells wet in BOTH, inside the margin (the
    // border-drain formulations differ slightly by construction).
    u32 compared = 0;
    for (u32 row = 8; row < spec.n - 8; ++row) {
        for (u32 col = 8; col < spec.n - 8; ++col) {
            const size_t i = static_cast<size_t>(row) * spec.n + col;
            if (oracle.depth[i] < 0.02f || state.depth[i] < 0.02f) {
                continue;
            }
            ++compared;
            const f32 a = terrain[i] + oracle.depth[i];
            const f32 b = terrain[i] + state.depth[i];
            // The real-time drain cap (25%/substep, the anti-packet
            // stabilizer) shifts steep-cell equilibria by up to ~0.2 m
            // under this test's storm-grade rain — accepted drift
            // between the two kernels.
            CHECK(std::abs(a - b) < 0.25f);
        }
    }
    CHECK(compared > 50);
}

TEST_CASE("water sim: a pinned lake pours over its whole rim") {
    // A plateau lake (baked LakeSurface) pinned at level 310 next to a
    // 20 m drop: the reservoir never empties, so water pours over
    // every rim cell below the level — the wide waterfall — and keeps
    // pouring (infinite supply).
    const GridSpec spec = makeSpec(65, 2.0f);
    const auto plateau = [](f32 x, f32) {
        return x < 48.0f ? 305.0f : 285.0f; // basin floor | lower land
    };
    WaterSimState state;
    initWindow(state, spec, plateau, -1000.0f);
    render::WaterBodies bodies;
    render::LakeSurface lake;
    lake.level = 310.0f;
    lake.minX = 0.0f;
    lake.minZ = 0.0f;
    lake.maxX = 44.0f;
    lake.maxZ = 128.0f;
    // Masked (generated) lake — maskless ponds are excluded from
    // pinning; the interior erosion trims one mask texel off the rim.
    lake.maskTexel = 4.0f;
    lake.maskWidth = 12;
    lake.maskHeight = 33;
    lake.mask.assign(static_cast<size_t>(lake.maskWidth) *
                         lake.maskHeight,
                     1);
    bodies.lakes.push_back(lake);
    pinLakes(state, bodies);
    // Seeding: the FULL baked footprint is wet immediately — even the
    // eroded rim ring outside the pins (the "flan" fix: the lake must
    // occupy its baked shape, not creep toward it at the weir rate).
    {
        // This synthetic lake stands 5 m above ALL surrounding
        // ground, so every covered cell near the mask edge counts as
        // past-crest for the overhang guard and the seed ring is
        // empty — the reservoir CORE must still be pinned (the
        // immediate-footprint seeding of a realistic shore is proven
        // in the crest-overhang case below).
        const size_t rimCell = 30u * spec.n + 16u;
        CHECK(state.pinned[rimCell] >
              render::terrain::kWaterInfoDry + 1.0f);
    }
    WaterSimParams params = closedParams();
    params.borderDrainPerSecond = 0.9f;
    stepWindow(state, params, {}, 1200);
    // The lake HOLDS its pinned level (the supply is bounded on the
    // way out — reservoirOutflow — not by starving the refill, which
    // collapsed the surface into a drawdown cone).
    const size_t inLake = 30u * spec.n + 10u;
    CHECK(state.terrain[inLake] + state.depth[inLake] ==
          doctest::Approx(310.0f).epsilon(0.001));
    // ...and the lower land is wet across a WIDE front (many rows).
    u32 wetRows = 0;
    for (u32 row = 2; row < spec.n - 2; ++row) {
        f32 rowWet = 0.0f;
        for (u32 col = 28; col < spec.n; ++col) {
            rowWet +=
                state.depth[static_cast<size_t>(row) * spec.n + col];
        }
        if (rowWet > 0.05f) {
            ++wetRows;
        }
    }
    CHECK(wetRows > 40);
    // The lake PUBLISHES (the sim mesh is the lake inside the rect —
    // a publish-dry variant floated the baked overhang over the fall
    // lip), and the pour over the rim is in the snapshot with it.
    WaterSimSnapshot snap;
    extractSnapshot(state, params, snap);
    CHECK(snap.depth[inLake] > 0.0f);
    u32 pourWet = 0;
    for (u32 row = 0; row < spec.n; ++row) {
        for (u32 col = 26; col < spec.n; ++col) {
            if (snap.depth[static_cast<size_t>(row) * spec.n + col] >
                0.0f) {
                ++pourWet;
            }
        }
    }
    CHECK(pourWet > 100);
}

TEST_CASE("water sim: extraction builds one closed, column-capped mesh") {
    // A flat plateau with a 10x10-cell basin: the flood start fills it
    // to its spill, giving a wet block with a full shoreline.
    const GridSpec spec = makeSpec(65, 2.0f);
    const auto basin = [](f32 x, f32 z) {
        const bool in = x >= 40.0f && x <= 58.0f && z >= 40.0f &&
                        z <= 58.0f;
        return in ? 298.5f : 300.0f;
    };
    WaterSimState state;
    initWindow(state, spec, basin, -1000.0f);
    // Standing water comes from the bake (pins) or sources, never
    // from init — fill the basin explicitly for this geometry test.
    for (size_t i = 0; i < spec.cells(); ++i) {
        if (state.terrain[i] < 299.0f) {
            state.depth[i] = 299.9f - state.terrain[i];
        }
    }
    WaterSimParams params = closedParams();
    WaterSimSnapshot snap;
    extractSnapshot(state, params, snap);

    u32 wet = 0;
    f32 minColumnFloor = 1.0e9f;
    for (size_t i = 0; i < snap.depth.size(); ++i) {
        if (snap.depth[i] > 0.0f) {
            ++wet;
            minColumnFloor = glm::min(
                minColumnFloor, snap.surface[i] - snap.depth[i]);
        }
    }
    REQUIRE(wet > 50);
    REQUIRE(!snap.meshIndices.empty());
    // Watertight accounting: tops (6 indices per wet cell) + one side
    // quad (6 indices) per wet/dry boundary edge with a real drop.
    u32 sides = 0;
    const auto wetAt = [&](i32 c, i32 r) {
        return c >= 0 && r >= 0 && c < static_cast<i32>(spec.n) &&
               r < static_cast<i32>(spec.n) &&
               snap.depth[static_cast<size_t>(r) * spec.n + c] > 0.0f;
    };
    for (u32 r = 0; r < spec.n; ++r) {
        for (u32 c = 0; c < spec.n; ++c) {
            if (!wetAt(static_cast<i32>(c), static_cast<i32>(r))) {
                continue;
            }
            const i32 dirs[4][2] = {
                { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }
            };
            for (const auto& d : dirs) {
                const i32 nc = static_cast<i32>(c) + d[0];
                const i32 nr = static_cast<i32>(r) + d[1];
                if (nc < 0 || nr < 0 || nc >= static_cast<i32>(spec.n) ||
                    nr >= static_cast<i32>(spec.n)) {
                    continue;
                }
                if (!wetAt(nc, nr)) {
                    ++sides; // basin wall: ground drop guarantees one
                }
            }
        }
    }
    CHECK(snap.meshIndices.size() ==
          static_cast<size_t>(wet) * 6 + static_cast<size_t>(sides) * 6);
    // Column cap: no vertex sinks below any wet column's floor.
    f32 minY = 1.0e9f;
    for (size_t v = 0; v + 4 < snap.meshVerts.size(); v += 5) {
        CHECK(std::isfinite(snap.meshVerts[v + 1]));
        minY = glm::min(minY, snap.meshVerts[v + 1]);
    }
    CHECK(minY >= minColumnFloor - 0.16f);
    // Determinism: a second extraction is bit-identical.
    WaterSimSnapshot again;
    extractSnapshot(state, params, again);
    CHECK(snap.meshVerts == again.meshVerts);
    CHECK(snap.meshIndices == again.meshIndices);
}

TEST_CASE("water sim: mask overhang past an eroded crest never seeds") {
    // A REALISTIC shelf pond (level ~1.5 m over its shore — a baked
    // level sits at its spill) whose mask overruns the crest of a
    // 30 m drop by two sim cells: seeding those cells planted a full
    // water column jutting over the falls (the "lake cube"). They
    // must stay dry after pinLakes; the basin still seeds/pins.
    const GridSpec spec = makeSpec(65, 2.0f);
    const auto shelf = [](f32 x, f32) {
        return x < 44.0f ? 300.0f : 270.0f;
    };
    WaterSimState state;
    initWindow(state, spec, shelf, -1000.0f);
    render::WaterBodies bodies;
    render::LakeSurface lake;
    lake.level = 301.5f;
    lake.minX = 0.0f;
    lake.minZ = 0.0f;
    lake.maxX = 48.0f; // overhangs the x=44 crest
    lake.maxZ = 128.0f;
    lake.maskTexel = 4.0f;
    lake.maskWidth = 12;
    lake.maskHeight = 32;
    lake.mask.assign(static_cast<size_t>(lake.maskWidth) *
                         lake.maskHeight,
                     1);
    bodies.lakes.push_back(lake);
    pinLakes(state, bodies);
    const size_t overhang = 30u * spec.n + 22u;  // x=44, terrain 270
    const size_t crestEdge = 30u * spec.n + 21u; // x=42, near crest
    const size_t interior = 30u * spec.n + 15u;  // x=30, mask core
    const size_t seedRim = 63u * spec.n + 15u;   // z=126: seeded, not
                                                 // pinned (erosion)
    // Past-crest cells carry the thin connective FILM (the lake-to-
    // fall interpolation), never the full column (the "cube" would be
    // 31.5 m at the overhang), and never a pin.
    CHECK(state.depth[overhang] ==
          doctest::Approx(0.08f).epsilon(0.01));
    CHECK(state.depth[crestEdge] ==
          doctest::Approx(0.08f).epsilon(0.01));
    CHECK(state.pinned[overhang] <
          render::terrain::kWaterInfoDry + 1.0f);
    CHECK(state.pinned[interior] ==
          doctest::Approx(301.5f).epsilon(0.001));
    CHECK(state.depth[seedRim] ==
          doctest::Approx(1.5f).epsilon(0.01));
}

TEST_CASE("water sim: scroll strips refill from breadcrumbs") {
    // Walking away scrolls (never evicts): breadcrumb states dropped
    // along the way let the ENTERING strips remember their water —
    // copied, not invented (the dry-strips doctrine bans invention).
    const GridSpec spec = makeSpec(65, 2.0f); // covers [0, 128]
    const auto flat = [](f32, f32) { return 300.0f; };
    WaterSimState state;
    initWindow(state, spec, flat, -1000.0f);
    auto crumb = std::make_shared<WaterSimState>();
    initWindow(*crumb, makeSpec(65, 2.0f, 64.0f, 0.0f), flat,
               -1000.0f); // covers [64, 192]
    // wetMask is lazily sized at the first extraction — size it like
    // a live (extracted-at-least-once) window would have it.
    crumb->wetMask.assign(crumb->spec.cells(), 0);
    for (u32 row = 0; row < crumb->spec.n; ++row) {
        for (u32 col = 0; col < crumb->spec.n; ++col) {
            const f32 x = crumb->spec.x(col);
            if (x >= 140.0f && x <= 150.0f) {
                const size_t j =
                    static_cast<size_t>(row) * crumb->spec.n + col;
                crumb->depth[j] = 2.0f;
                crumb->fE[j] = 0.5f;
                crumb->wetMask[j] = 1;
            }
        }
    }
    vector<sptr<const WaterSimState>> crumbs { crumb };
    scrollWindow(state, 16, 0, flat, -1000.0f, &crumbs); // -> [32,160]
    const auto idxAt = [&](f32 x) {
        return 30u * state.spec.n +
               static_cast<u32>((x - state.spec.originX) / 2.0f);
    };
    CHECK(state.depth[idxAt(144.0f)] == 2.0f); // remembered
    CHECK(state.fE[idxAt(144.0f)] == 0.5f);    // pipes too (moving)
    CHECK(state.wetMask[idxAt(144.0f)] == 1);
    CHECK(state.depth[idxAt(156.0f)] == 0.0f); // crumb dry there: dry
}

TEST_CASE("water sim: chooseCachedWindow picks the best resumable state") {
    // The session LRU lever (docs/WATER-RENDER.md §4): a cached
    // window resumes via scrollWindow when it overlaps the target
    // enough; knob changes (n/texel) disqualify; best overlap wins.
    const auto specAt = [](f32 ox, f32 oz, u32 n = 65, f32 t = 2.0f) {
        GridSpec s;
        s.originX = ox;
        s.originZ = oz;
        s.texelSize = t;
        s.n = n;
        return s;
    };
    const GridSpec target = specAt(64.0f, -32.0f);
    // Empty cache: no pick.
    CHECK(chooseCachedWindow({}, target).index == -1);
    // Exact match: shift 0/0.
    {
        const auto pick =
            chooseCachedWindow({ specAt(64.0f, -32.0f) }, target);
        CHECK(pick.index == 0);
        CHECK(pick.dCol == 0);
        CHECK(pick.dRow == 0);
    }
    // Overlapping candidate: picked, with the scroll shift.
    {
        const auto pick =
            chooseCachedWindow({ specAt(0.0f, 0.0f) }, target);
        CHECK(pick.index == 0);
        CHECK(pick.dCol == 32);  // (64-0)/2
        CHECK(pick.dRow == -16); // (-32-0)/2
    }
    // Too little overlap (a 9-column sliver, ~14 %) or knob mismatch:
    // rejected.
    CHECK(chooseCachedWindow({ specAt(-48.0f, -32.0f) }, target)
              .index == -1);
    CHECK(chooseCachedWindow({ specAt(64.0f, -32.0f, 33) }, target)
              .index == -1);
    CHECK(chooseCachedWindow({ specAt(64.0f, -32.0f, 65, 4.0f) },
                             target)
              .index == -1);
    // Best of several: the closer window wins.
    {
        const auto pick = chooseCachedWindow(
            { specAt(0.0f, 0.0f), specAt(60.0f, -30.0f) }, target);
        CHECK(pick.index == 1);
        CHECK(pick.dCol == 2);
        CHECK(pick.dRow == -1);
    }
}

TEST_CASE("water sim: a tile seam between lake pieces never films") {
    // A big lake is baked as PER-TILE pieces: past one piece's bbox
    // lies the SAME lake, not the void. The crest guard must see the
    // sibling piece — without it, every interior seam grew a 16 m
    // band of 8 cm film ("the floor rising as if the lake ended",
    // measured dev, the chevron screenshot).
    const GridSpec spec = makeSpec(65, 2.0f);
    const auto basin = [](f32, f32) { return 294.0f; };
    WaterSimState state;
    initWindow(state, spec, basin, -1000.0f);
    render::WaterBodies bodies;
    const auto piece = [](f32 minX, f32 maxX) {
        render::LakeSurface p;
        p.level = 300.0f;
        p.minX = minX;
        p.minZ = 0.0f;
        p.maxX = maxX;
        p.maxZ = 124.0f;
        p.maskTexel = 4.0f;
        p.maskWidth = static_cast<u32>((maxX - minX) / 4.0f);
        p.maskHeight = 31;
        p.mask.assign(static_cast<size_t>(p.maskWidth) * p.maskHeight,
                      1);
        return p;
    };
    bodies.lakes.push_back(piece(0.0f, 64.0f));
    bodies.lakes.push_back(piece(64.0f, 124.0f));
    pinLakes(state, bodies);
    // Cell near the seam, inside piece A, deep basin: FULL column
    // (not pinned: its pin-erosion probe crosses the seam).
    const size_t nearSeam = 30u * spec.n + 31u; // (62, 60)
    CHECK(state.terrain[nearSeam] + state.depth[nearSeam] ==
          doctest::Approx(300.0f).epsilon(0.001));
}

TEST_CASE("water sim: a mask hole mid-lake never craters the seed") {
    // The 8 m mask can carry uncovered texels OVER deep water
    // (rasterization holes, concave bays, island rings). The crest
    // guard must not fire there — keying it on mere coverage seeded a
    // 4-cell film crater around every such texel: circles showing the
    // floor at the freshly scrolled window margins (measured dev).
    // Only ground below the level AND outside the lake's bbox is the
    // void past a crest.
    const GridSpec spec = makeSpec(65, 2.0f);
    const auto basin = [](f32, f32) { return 294.0f; };
    WaterSimState state;
    initWindow(state, spec, basin, -1000.0f);
    render::WaterBodies bodies;
    render::LakeSurface lake;
    lake.level = 300.0f;
    lake.minX = 0.0f;
    lake.minZ = 0.0f;
    lake.maxX = 124.0f;
    lake.maxZ = 124.0f;
    lake.maskTexel = 4.0f;
    lake.maskWidth = 31;
    lake.maskHeight = 31;
    lake.mask.assign(static_cast<size_t>(lake.maskWidth) *
                         lake.maskHeight,
                     1);
    lake.mask[15u * lake.maskWidth + 15u] = 0; // hole near (60, 60)
    bodies.lakes.push_back(lake);
    pinLakes(state, bodies);
    // Covered cell right next to the hole — close enough that the pin
    // erosion rejects it (so the BFS seed sets its depth, pins carry
    // none before the first step): FULL column, not the film.
    const size_t nearHole = 29u * spec.n + 27u; // (54, 58)
    CHECK(state.pinned[nearHole] < render::terrain::kWaterInfoDry + 1.0f);
    CHECK(state.terrain[nearHole] + state.depth[nearHole] ==
          doctest::Approx(300.0f).epsilon(0.001));
}

TEST_CASE("water sim: a narrow lake still pins and seeds (fallback)") {
    // One mask texel wide: the interior erosion leaves NO pin — the
    // fallback pins the deepest covered cell so the BFS still seeds
    // the whole trench (the remaining-"flan" fix).
    const GridSpec spec = makeSpec(65, 2.0f);
    const auto trench = [](f32 x, f32 z) {
        const bool in = x >= 60.0f && x <= 66.0f && z >= 20.0f &&
                        z <= 100.0f;
        return in ? 296.0f : 300.0f;
    };
    WaterSimState state;
    initWindow(state, spec, trench, -1000.0f);
    render::WaterBodies bodies;
    render::LakeSurface lake;
    lake.level = 299.5f;
    lake.minX = 58.0f;
    lake.minZ = 18.0f;
    lake.maxX = 68.0f;
    lake.maxZ = 102.0f;
    lake.maskTexel = 8.0f;
    lake.maskWidth = 3;
    lake.maskHeight = 12;
    lake.mask.assign(static_cast<size_t>(lake.maskWidth) *
                         lake.maskHeight,
                     1);
    bodies.lakes.push_back(lake);
    pinLakes(state, bodies);
    u32 pinnedCount = 0;
    f64 seeded = 0.0;
    for (size_t i = 0; i < spec.cells(); ++i) {
        if (state.pinned[i] > render::terrain::kWaterInfoDry + 1.0f) {
            ++pinnedCount;
        }
        seeded += state.depth[i];
    }
    CHECK(pinnedCount >= 1);
    CHECK(seeded > 10.0); // the trench holds water immediately
}

TEST_CASE("water sim: no phantom lakes — enclosed valleys start dry") {
    // A deep bowl whose only exit lies past the rim: without a baked
    // lake (pin) or a source, the window must NOT invent water — the
    // old priority-flood warm start filled such valleys to their pass
    // (a 138 m / 9.5M m³ phantom lake, replay-measured).
    const GridSpec spec = makeSpec(65, 2.0f);
    WaterSimState state;
    initWindow(state, spec, bowlHeight, -1000.0f);
    CHECK(totalVolume(state) == 0.0);
    WaterSimParams params = closedParams();
    params.rainRate = 1.0e-5f;
    stepWindow(state, params, {}, 300);
    // Only the rain that actually fell may stand (300 substeps at
    // 1/30 s = 10 s of 1e-5 m/s over the window).
    const f64 rained = 1.0e-5 * 10.0 *
                       static_cast<f64>(spec.cells()) * 4.0;
    CHECK(totalVolume(state) <= rained * 1.01);
}

TEST_CASE("water sim: wetness hysteresis is sticky") {
    const GridSpec spec = makeSpec(33, 2.0f);
    WaterSimState state;
    initWindow(
        state, spec, [](f32, f32) { return 300.0f; }, -1000.0f);
    const WaterSimParams params = closedParams();
    // A 2x2 block (connectivity) hovering around the thresholds.
    const size_t block[4] = { 16u * spec.n + 16u, 16u * spec.n + 17u,
                              17u * spec.n + 16u, 17u * spec.n + 17u };
    WaterSimSnapshot snap;
    const auto setBlock = [&](f32 d) {
        for (const size_t i : block) {
            state.depth[i] = d;
        }
    };
    setBlock(0.02f); // below the ENTRY threshold
    extractSnapshot(state, params, snap);
    CHECK(snap.depth[block[0]] == 0.0f);
    setBlock(0.035f); // above entry: turns wet
    extractSnapshot(state, params, snap);
    CHECK(snap.depth[block[0]] > 0.0f);
    setBlock(0.02f); // dips below entry but above EXIT: stays wet
    extractSnapshot(state, params, snap);
    CHECK(snap.depth[block[0]] > 0.0f);
    setBlock(0.005f); // below exit: dries
    extractSnapshot(state, params, snap);
    CHECK(snap.depth[block[0]] == 0.0f);
}

TEST_CASE("water sim: kernel perf gate") {
    // The CPU-realtime premise (plan option C): the kernel must stay
    // well under 100 ns/cell/substep on a 256-class window. Generous
    // bound so debug-adjacent machines pass; a de-optimization still
    // trips it.
    const GridSpec spec = makeSpec(257, 2.0f);
    WaterSimState state;
    initWindow(state, spec, bowlHeight, -1000.0f);
    WaterSimParams params = closedParams();
    params.rainRate = 1.0e-5f;
    stepWindow(state, params, {}, 10); // warm caches
    const auto start = std::chrono::steady_clock::now();
    const u32 iters = 200;
    stepWindow(state, params, {}, iters);
    const f64 seconds =
        std::chrono::duration<f64>(std::chrono::steady_clock::now() -
                                   start)
            .count();
    const f64 nsPerCellIter =
        seconds * 1.0e9 /
        (static_cast<f64>(spec.cells()) * static_cast<f64>(iters));
    MESSAGE("water sim kernel: ", nsPerCellIter, " ns/cell/substep");
    CHECK(nsPerCellIter < 100.0);
}
