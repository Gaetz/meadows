#include <doctest/doctest.h>

#include <cmath>

#include "engine/terrain/Noise.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"

// Seconds-to-minutes cases (bakes, sims): the "slow" suite, skipped by
// the fast run (-tse=slow), kept by the full one (docs/AUDIT/U9-tests.md).
TEST_SUITE_BEGIN("slow");

// Stage S1 (macro synthesis): elevation tiers, terracing, coast profile.
// Everything here must be deterministic — the sandbox bakes tiles from
// these functions and caches the bytes.

using namespace render::terraingen;

namespace {

// Test control source: constant fields, sea on the x < 0 half-plane when
// `halfSea` is set.
struct FixedControls final : ControlSource {
    f32 tier { 0.0f };
    f32 uplift { 0.0f };
    f32 hardness { 0.5f };
    bool halfSea { false };

    ControlSample at(f32 x, f32) const override {
        ControlSample s;
        s.tier = tier;
        s.uplift = uplift;
        s.hardness = hardness;
        s.sea = halfSea && x < 0.0f;
        return s;
    }
};

GridSpec spec1km(f32 originX = -512.0f, f32 originZ = -512.0f) {
    return GridSpec { originX, originZ, 8.0f, 129 };
}

} // namespace

TEST_CASE("macro synthesis is deterministic") {
    FixedControls controls;
    controls.tier = 1.0f;
    const MacroParams params;
    const MacroResult a = synthesizeMacro(controls, spec1km(), params, 7);
    const MacroResult b = synthesizeMacro(controls, spec1km(), params, 7);
    CHECK(a.height == b.height); // bit-exact
    CHECK(a.seaDist == b.seaDist);

    const MacroResult c = synthesizeMacro(controls, spec1km(), params, 8);
    CHECK(a.height != c.height); // the seed matters
}

TEST_CASE("recurve: identity by default, monotone and coast-fixed set") {
    const MacroParams p;
    // Default control points: bit-exact identity everywhere.
    CHECK(recurveLand(p, 250.0f) == 250.0f);
    CHECK(recurveLand(p, p.seaLevel - 5.0f) == p.seaLevel - 5.0f);

    MacroParams curved = p;
    curved.recurveMid = 0.32f; // flatter plains, steeper rises
    // Sea, shoreline and above-span land untouched.
    CHECK(recurveLand(curved, p.seaLevel) == p.seaLevel);
    CHECK(recurveLand(curved, p.seaLevel - 10.0f) == p.seaLevel - 10.0f);
    const f32 top = p.seaLevel + curved.recurveSpan + 5.0f;
    CHECK(recurveLand(curved, top) == top);
    // Monotone over the whole band.
    f32 previous = recurveLand(curved, p.seaLevel);
    for (f32 h = p.seaLevel + 1.0f;
         h <= p.seaLevel + curved.recurveSpan; h += 2.0f) {
        const f32 r = recurveLand(curved, h);
        CHECK(r >= previous);
        previous = r;
    }
    // Mid pulled down: mid-band land sits lower than before.
    CHECK(recurveLand(curved, p.seaLevel + 350.0f) <
          p.seaLevel + 350.0f);

    // The synthesized grid follows: land texels move, sea texels don't.
    FixedControls controls;
    controls.tier = 1.5f;
    controls.halfSea = true;
    const MacroResult flat = synthesizeMacro(controls, spec1km(), p, 7);
    const MacroResult bent =
        synthesizeMacro(controls, spec1km(), curved, 7);
    u32 landMoved = 0;
    u32 seaMoved = 0;
    for (size_t i = 0; i < flat.height.size(); ++i) {
        if (flat.height[i] == bent.height[i]) {
            continue;
        }
        (flat.seaDist[i] > 0.0f ? landMoved : seaMoved) += 1;
    }
    CHECK(landMoved > 0);
    CHECK(seaMoved == 0);
}

TEST_CASE("a tier floor holds its altitude within its relief amplitude") {
    FixedControls controls;
    controls.tier = 1.0f; // hills
    const MacroParams params;
    const TierLevel& hills = params.tiers[1];
    const MacroResult r = synthesizeMacro(controls, spec1km(), params, 7);
    for (const f32 h : r.height) {
        CHECK(h > hills.altitude - hills.reliefAmplitude - 1.0f);
        CHECK(h < hills.altitude + hills.reliefAmplitude + 1.0f);
    }
}

TEST_CASE("full terracing with flat relief snaps to strata multiples") {
    FixedControls controls;
    controls.tier = 2.0f; // mesa tier
    MacroParams params;
    params.tiers[2].reliefAmplitude = 0.0f;
    params.tiers[2].terrace = 1.0f;
    const MacroResult r = synthesizeMacro(controls, spec1km(), params, 7);
    // Constant input -> one stratum, exactly on a terraceStep multiple.
    const f32 h = r.height[0];
    const f32 strata = h / params.terraceStep;
    CHECK(std::abs(strata - std::round(strata)) < 1e-3f);
    for (const f32 v : r.height) {
        CHECK(v == doctest::Approx(h));
    }
}

TEST_CASE("beach coasts ramp to the waterline, cliff coasts hold the rim") {
    MacroParams params;
    const GridSpec spec = spec1km();

    FixedControls beach;
    beach.tier = 0.0f;
    beach.halfSea = true;
    const MacroResult rb = synthesizeMacro(beach, spec, params, 7);
    const auto at = [&](const MacroResult& r, f32 x, f32 z) {
        const u32 col = static_cast<u32>((x - spec.originX) / spec.texelSize);
        const u32 row = static_cast<u32>((z - spec.originZ) / spec.texelSize);
        return r.height[static_cast<size_t>(row) * spec.n + col];
    };
    // Deep water is deep, the far shore side reaches land height, and the
    // waterline sits at shoreHeight above sea level.
    CHECK(at(rb, -496.0f, 0.0f) < params.seaLevel - 4.0f);
    // The first beach texel rides the story relief's slope (+/-75 m
    // over 500 m): within a few meters of the waterline.
    CHECK(at(rb, 8.0f, 0.0f) ==
          doctest::Approx(params.seaLevel + params.shoreHeight)
              .epsilon(0.3));
    CHECK(at(rb, 496.0f, 0.0f) > params.seaLevel + 0.5f);

    FixedControls cliff;
    cliff.tier = 3.0f; // above cliffTierEnd: no beach ramp
    cliff.halfSea = true;
    const MacroResult rc = synthesizeMacro(cliff, spec, params, 7);
    // Just inside the rim the land keeps its highland altitude: a sea
    // cliff, tens of meters above the water at the very shore. Offshore
    // the shelf profile is already below sea level (the first meters stay
    // near the waterline by continuity).
    CHECK(at(rc, 8.0f, 0.0f) > params.seaLevel + 60.0f);
    CHECK(at(rc, -48.0f, 0.0f) < params.seaLevel);
}

TEST_CASE("two-stage ocean: shore ramp, coastal plateau, deep floor") {
    // Wide grid: the talus runs to seaFalloff (2.5 km) — spec1km cannot
    // hold the full profile.
    MacroParams params;
    const GridSpec spec { -6144.0f, -1024.0f, 16.0f, 513 };
    FixedControls beach;
    beach.halfSea = true;
    const MacroResult r = synthesizeMacro(beach, spec, params, 7);
    const auto at = [&](f32 x) {
        const u32 col =
            static_cast<u32>((x - spec.originX) / spec.texelSize);
        const u32 row =
            static_cast<u32>((0.0f - spec.originZ) / spec.texelSize);
        return r.height[static_cast<size_t>(row) * spec.n + col];
    };
    // The plateau: depth settles at shelfDepth by mid-band and STAYS
    // there until shelfEnd.
    CHECK(at(-350.0f) ==
          doctest::Approx(params.seaLevel - params.shelfDepth)
              .epsilon(0.25));
    CHECK(at(-550.0f) ==
          doctest::Approx(params.seaLevel - params.shelfDepth)
              .epsilon(0.25));
    // The talus: past shelfEnd the floor dives toward seaFloor.
    CHECK(at(-1500.0f) < params.seaLevel - 35.0f);
    CHECK(at(-3500.0f) ==
          doctest::Approx(params.seaFloor).epsilon(0.1));
    // Monotone seaward along the profile (no bumps between stages).
    f32 prev = at(-16.0f);
    for (f32 x = -32.0f; x >= -4000.0f; x -= 16.0f) {
        const f32 h = at(x);
        CHECK(h <= prev + 1.0e-3f);
        prev = h;
    }
    // Cliff coasts contract every band: the deep floor arrives sooner.
    FixedControls cliff;
    cliff.tier = 3.0f;
    cliff.halfSea = true;
    const MacroResult rc = synthesizeMacro(cliff, spec, params, 7);
    const auto atc = [&](f32 x) {
        const u32 col =
            static_cast<u32>((x - spec.originX) / spec.texelSize);
        const u32 row =
            static_cast<u32>((0.0f - spec.originZ) / spec.texelSize);
        return rc.height[static_cast<size_t>(row) * spec.n + col];
    };
    CHECK(atc(-1500.0f) < at(-1500.0f) + 1.0e-3f);
    CHECK(atc(-1200.0f) ==
          doctest::Approx(params.seaFloor).epsilon(0.1));
}

TEST_CASE("uplift is zero at sea and bounded on land") {
    ProceduralControlParams pc;
    pc.seed = 99;
    const ProceduralControls controls { pc };
    // 64 km: the massif belts run at ~26 km, a window must hold one.
    const GridSpec spec { -32768.0f, -32768.0f, 256.0f, 257 };
    const MacroParams params;
    const MacroResult r = synthesizeMacro(controls, spec, params, pc.seed);
    f32 maxUplift = 0.0f;
    for (size_t i = 0; i < r.uplift.size(); ++i) {
        CHECK(r.uplift[i] >= 0.0f);
        CHECK(r.uplift[i] <= 1.0f);
        if (r.seaDist[i] < 0.0f) {
            CHECK(r.uplift[i] == 0.0f);
        }
        maxUplift = std::max(maxUplift, r.uplift[i]);
    }
    // Somewhere in 64x64 km a range wants to rise.
    CHECK(maxUplift > 0.2f);
}

TEST_CASE("control sample with out-world-sample is bit-identical") {
    // The two-output overload must be THE same evaluation: same sample
    // fields bitwise, and the handed-back world sample equal to a
    // direct call — the dedupe in macroHeightAnalytic rests on it.
    ProceduralControlParams pc;
    pc.seed = 777;
    const ProceduralControls controls { pc };
    for (i32 gz = -8; gz <= 8; ++gz) {
        for (i32 gx = -8; gx <= 8; ++gx) {
            const f32 x = static_cast<f32>(gx) * 3777.0f;
            const f32 z = static_cast<f32>(gz) * 2913.0f;
            const ControlSample a = controls.at(x, z);
            WorldSample w;
            const ControlSample b = controls.at(x, z, w);
            REQUIRE(a.sea == b.sea);
            REQUIRE(a.biome == b.biome);
            REQUIRE(a.tier == b.tier);
            REQUIRE(a.uplift == b.uplift);
            REQUIRE(a.plateau == b.plateau);
            REQUIRE(a.hillRelief == b.hillRelief);
            REQUIRE(a.gentle == b.gentle);
            REQUIRE(a.calm == b.calm);
            REQUIRE(a.reliefScale == b.reliefScale);
            REQUIRE(a.axisCos == b.axisCos);
            REQUIRE(a.axisSin == b.axisSin);
            REQUIRE(a.axisStrength == b.axisStrength);
            REQUIRE(a.trunk == b.trunk);
            REQUIRE(a.trunkDepth == b.trunkDepth);
            REQUIRE(a.hardness == b.hardness);
            REQUIRE(a.base == b.base);
            REQUIRE(a.hasBase == b.hasBase);
            REQUIRE(a.bedDepth == b.bedDepth);
            const WorldSample direct =
                worldSampleAt(controls.params().world, x, z);
            REQUIRE(w.continent == direct.continent);
            REQUIRE(w.base == direct.base);
            REQUIRE(w.massif == direct.massif);
        }
    }
}

TEST_CASE("procedural controls carve both sea and high ground") {
    ProceduralControlParams pc;
    pc.seed = 4242;
    const ProceduralControls controls { pc };
    u32 seaCount = 0;
    u32 highCount = 0;
    for (i32 gz = -16; gz <= 16; ++gz) {
        for (i32 gx = -16; gx <= 16; ++gx) {
            const ControlSample s = controls.at(
                static_cast<f32>(gx) * 4000.0f,
                static_cast<f32>(gz) * 4000.0f);
            if (s.sea) {
                ++seaCount;
            }
            if (s.tier > 2.0f) {
                ++highCount;
            }
        }
    }
    CHECK(seaCount > 0);
    CHECK(highCount > 0);
}

TEST_CASE("biome ids resolve per texel, not on the coarse lattice") {
    // A biome border at x = 37 — deliberately NOT a 64 m lattice
    // multiple: the macro grid must carry it at texel resolution
    // (nearest-sampling the id on the coarse control lattice drew 64 m
    // axis-aligned biome stairs).
    struct BiomeControls final : ControlSource {
        ControlSample at(f32 x, f32) const override {
            ControlSample s;
            s.tier = 1.0f;
            s.biome = x > 37.0f ? 2 : 0;
            return s;
        }
    } controls;
    const GridSpec spec = spec1km();
    const MacroResult r =
        synthesizeMacro(controls, spec, MacroParams {}, 7);
    for (const u32 row : { 0u, 64u, 128u }) {
        for (u32 col = 0; col < spec.n; ++col) {
            const u8 expected = spec.x(col) > 37.0f ? 2 : 0;
            CHECK(r.biome[static_cast<size_t>(row) * spec.n + col] ==
                  expected);
        }
    }
}

TEST_CASE("procedural biomeIdAt agrees with the full control sample") {
    ProceduralControlParams pc;
    pc.seed = 1337;
    const ProceduralControls controls { pc };
    for (i32 gz = -8; gz <= 8; ++gz) {
        for (i32 gx = -8; gx <= 8; ++gx) {
            const f32 x = static_cast<f32>(gx) * 1730.0f;
            const f32 z = static_cast<f32>(gz) * 1730.0f;
            const ControlSample s = controls.at(x, z);
            CHECK(controls.biomeIdAt(x, z, s.tier) == s.biome);
        }
    }
}

TEST_CASE("the analytic macro matches the tier floors away from shore") {
    ProceduralControlParams pc;
    pc.seed = 31;
    const ProceduralControls controls { pc };
    const MacroParams params;
    // Deterministic and bounded by the highest tier + its relief.
    f32 maxSeen = -1000.0f;
    for (i32 gz = -12; gz <= 12; ++gz) {
        for (i32 gx = -12; gx <= 12; ++gx) {
            const f32 x = static_cast<f32>(gx) * 700.0f;
            const f32 z = static_cast<f32>(gz) * 700.0f;
            const f32 a = macroHeightAnalytic(controls, params, x, z);
            const f32 b = macroHeightAnalytic(controls, params, x, z);
            CHECK(a == b);
            CHECK(a >= params.seaFloor - 1.0f);
            // Ceiling: the top étage + the massif lift + relief + the
            // tallest piece + the massif crests.
            const WorldLayerParams& world = controls.params().world;
            const RhythmParams& rhythm = controls.params().rhythm;
            CHECK(a <= world.etageAltitude[3] + world.massifLift +
                           params.tiers.back().reliefAmplitude +
                           glm::max(rhythm.pieceHeightByEtage[3][1],
                                    rhythm.storyMountainAmplitude) +
                           rhythm.crestAmplitudeByEtage[3] + 1.0f);
            maxSeen = std::max(maxSeen, a);
        }
    }
    CHECK(maxSeen > params.seaLevel); // some land exists
}

TEST_CASE("hard-rock coasts cliff into the sea, soft coasts beach") {
    FixedControls controls;
    controls.tier = 0.4f; // low country: the tier band alone never cliffs
    controls.halfSea = true;
    const MacroParams params;

    controls.hardness = 0.9f;
    const MacroResult hard =
        synthesizeMacro(controls, spec1km(), params, 11);
    controls.hardness = 0.1f;
    const MacroResult soft =
        synthesizeMacro(controls, spec1km(), params, 11);

    const auto at = [&](const MacroResult& r, f32 x, f32 z) {
        const u32 col = static_cast<u32>(
            std::lround((x - r.spec.originX) / r.spec.texelSize));
        const u32 row = static_cast<u32>(
            std::lround((z - r.spec.originZ) / r.spec.texelSize));
        return r.height[static_cast<size_t>(row) * r.spec.n + col];
    };
    // Just inland of the waterline: the hard coast keeps its altitude
    // to the rim, the soft coast is still on the beach ramp.
    CHECK(at(hard, 80.0f, 0.0f) > at(soft, 80.0f, 0.0f) + 3.0f);
    // Just offshore: the hard coast plunges deeper, sooner.
    CHECK(at(hard, -240.0f, 0.0f) < at(soft, -240.0f, 0.0f) - 3.0f);
    // Neutral hardness (the painted/test default) is the legacy coast.
    controls.hardness = 0.5f;
    const MacroResult neutral =
        synthesizeMacro(controls, spec1km(), params, 11);
    CHECK(at(neutral, 80.0f, 0.0f) ==
          doctest::Approx(at(soft, 80.0f, 0.0f)));
}

TEST_CASE("calm socles: plains and plateau tops join, ranges stay out") {
    ProceduralControlParams params;
    params.seed = 1337;
    const ProceduralControls controls { params };

    u64 land = 0, calmish = 0;
    f64 calmSum = 0.0;
    for (f32 z = -40000.0f; z <= 40000.0f; z += 200.0f) {
        for (f32 x = -40000.0f; x <= 40000.0f; x += 200.0f) {
            const ControlSample s = controls.at(x, z);
            if (s.sea) {
                continue;
            }
            ++land;
            calmSum += s.calm;
            if (s.calm > 0.6f) {
                ++calmish;
            }
            // The family is a superset of the corridors.
            CHECK(s.calm >= s.gentle - 1.0e-6f);
            // Strong orogeny is never a calm socle unless a corridor
            // or a plateau top says otherwise.
            if (s.uplift > 0.4f && s.gentle < 0.05f &&
                s.plateau < 90.0f) {
                CHECK(s.calm < 0.7f);
            }
        }
    }
    const f64 share = 100.0 * calmish / static_cast<f64>(land);
    MESSAGE("control-level calm>0.6: ", share, "% of land (mean ",
            calmSum / static_cast<f64>(land), ")");
    // Calm is the RULE (docs/PAYSAGE.md §7.5): the pieces, the story
    // mountains, the massifs and their flanks are the exceptions.
    CHECK(share > 25.0);
    CHECK(share < 90.0);

    // Deterministic: same params, same field.
    const ProceduralControls again { params };
    CHECK(again.at(1234.0f, -5678.0f).calm ==
          controls.at(1234.0f, -5678.0f).calm);
}

TEST_CASE("map border transitions: shared lines, coherent shapes") {
    // Chantier CARTES v2 (dev design): a transition belongs to the
    // BORDER LINE — hashed Sea/Ridges per segment, one pure meandering
    // shape both neighbour maps and the fallback share. Ranges rise
    // progressively (never a wall), sea arms drown both coasts, an
    // invalid spec is a strict identity — and everything reads the
    // world it stands on (the proximity rule + the Sea veto).
    using render::terraingen::applyMapGridShape;
    using render::terraingen::MacroParams;
    using render::terraingen::mapBorderStyleResolved;
    using render::terraingen::MapEdgeStyle;
    using render::terraingen::mapGridRidgeFactor;
    using render::terraingen::MapGridSpec;
    using render::terraingen::ProceduralControlParams;
    using render::terraingen::ProceduralControls;

    ProceduralControlParams pc;
    pc.seed = 1337;
    const ProceduralControls controls { pc };
    MacroParams macro;
    macro.seaLevel = 21.0f;

    MapGridSpec off;
    CHECK(applyMapGridShape(controls, macro, off, 123.0f, 456.0f,
                            78.9f) == doctest::Approx(78.9f));
    CHECK(mapGridRidgeFactor(controls, macro, off, 0.0f, 0.0f, 78.9f) ==
          doctest::Approx(0.0f));

    MapGridSpec spec;
    spec.valid = true;
    spec.seed = 1337;
    spec.mapSize = 8192.0f; // the game's 2x2 map
    spec.seaLevel = 21.0f;
    const f32 inland = 150.0f;
    // Past the band + wander, a line is invisible: the farthest point
    // that still belongs to this line's half of the map.
    const f32 far = spec.mapSize * 0.5f - 100.0f;
    // Sample ALONG mid-cell so the perpendicular lines contribute 0.
    const f32 along = spec.mapSize * 0.5f;

    // RESOLVED styles (the Sea veto applied): both kinds exist among
    // the nearby vertical lines, and both neighbours of a line see the
    // same style by construction.
    i32 ridgeLine = -1000;
    i32 seaLine = -1000;
    for (i32 i = -32; i < 32 && (ridgeLine < -64 || seaLine < -64);
         ++i) {
        const MapEdgeStyle s =
            mapBorderStyleResolved(controls, macro, spec, i, 0, true);
        if (s == MapEdgeStyle::Ridges && ridgeLine < -64) {
            ridgeLine = i;
        }
        if (s == MapEdgeStyle::Sea && seaLine < -64) {
            seaLine = i;
        }
    }
    REQUIRE(ridgeLine > -64);
    REQUIRE(seaLine > -64);

    const auto shape = [&](f32 x, f32 z, f32 h) {
        return applyMapGridShape(controls, macro, spec, x, z, h);
    };

    // Deep inside a map: untouched.
    CHECK(shape(spec.mapSize * 0.5f, along, inland) ==
          doctest::Approx(inland));

    // The Sea VETO itself: a segment whose analytic line is deep
    // inland never keeps a Sea proposal — every resolved Sea segment
    // has a genuinely coastal analytic line under it.
    {
        u32 oceanish = 0;
        for (u32 i = 0; i < 9; ++i) {
            const f32 sz = (static_cast<f32>(i) + 0.5f) / 9.0f *
                           spec.mapSize;
            if (render::terraingen::macroHeightAnalytic(
                    controls, macro,
                    static_cast<f32>(seaLine) * spec.mapSize, sz) <
                spec.seaLevel + 2.0f) {
                ++oceanish;
            }
        }
        CHECK(oceanish >= 3); // 0.34 * 9 samples
    }

    // Ridge line: the range stands somewhere in the (meandering) band
    // around the line, rising PROGRESSIVELY — no step exceeds what the
    // smooth profile allows — and fades back to the input well away.
    {
        const f32 lineX = static_cast<f32>(ridgeLine) * spec.mapSize;
        f32 peak = 0.0f;
        f32 previous = shape(lineX - 2200.0f, along, inland);
        for (f32 x = lineX - 2196.0f; x <= lineX + 2200.0f; x += 4.0f) {
            const f32 h = shape(x, along, inland);
            CHECK(std::abs(h - previous) < 4.0f); // progressive rise
            peak = glm::max(peak, h - inland);
            previous = h;
        }
        CHECK(peak >= 60.0f); // a real range on the line (crest varied)
        CHECK(shape(lineX - far, along, inland) == doctest::Approx(inland));
        CHECK(shape(lineX + far, along, inland) == doctest::Approx(inland));
        // The erosion keep exists on the range and nowhere far away
        // (it follows the varied crest, so a saddle can dip to ~0.3
        // of the profile).
        CHECK(mapGridRidgeFactor(controls, macro, spec, lineX, along,
                                 inland) > 0.12f);
        CHECK(mapGridRidgeFactor(controls, macro, spec, lineX + far,
                                 along, inland) == doctest::Approx(0.0f));
        // Crest height VARIES along the line (peaks and saddles — the
        // cols emerge from the system, they are not authored).
        f32 lo = 1.0e9f;
        f32 hi = -1.0e9f;
        for (f32 zz = 1000.0f; zz <= 7000.0f; zz += 100.0f) {
            f32 crest = 0.0f;
            for (f32 x = lineX - 1300.0f; x <= lineX + 1300.0f;
                 x += 50.0f) {
                crest = glm::max(crest, shape(x, zz, inland));
            }
            lo = glm::min(lo, crest);
            hi = glm::max(hi, crest);
        }
        CHECK(hi - lo > 40.0f);
    }

    // Sea line: a genuine channel — some point of the crossing sits at
    // open water (or an islet standing just clear), continuously.
    {
        const f32 lineX = static_cast<f32>(seaLine) * spec.mapSize;
        f32 low = 1.0e9f;
        f32 previous = shape(lineX - 2200.0f, along, inland);
        for (f32 x = lineX - 2196.0f; x <= lineX + 2200.0f; x += 4.0f) {
            const f32 h = shape(x, along, inland);
            CHECK(std::abs(h - previous) < 4.0f);
            low = glm::min(low, h);
            previous = h;
        }
        CHECK(low <= spec.seaLevel + 26.01f);
        CHECK(shape(lineX + far, along, inland) == doctest::Approx(inland));
    }

    // Coherence with the underlying terrain (the proximity rule): over
    // OPEN OCEAN the lattice is invisible — no range rises from the
    // sea, no islet chain appears, and a deep floor is never lifted
    // into a shelf.
    {
        const f32 ocean = spec.seaLevel - 120.0f;
        const f32 ridgeX = static_cast<f32>(ridgeLine) * spec.mapSize;
        const f32 seaX = static_cast<f32>(seaLine) * spec.mapSize;
        for (f32 dx = -1600.0f; dx <= 1600.0f; dx += 80.0f) {
            CHECK(shape(ridgeX + dx, along, ocean) ==
                  doctest::Approx(ocean));
            CHECK(shape(seaX + dx, along, ocean) ==
                  doctest::Approx(ocean));
            CHECK(mapGridRidgeFactor(controls, macro, spec, ridgeX + dx,
                                     along, ocean) ==
                  doctest::Approx(0.0f));
        }
        // A shallow coastal strip crossed by a ridge line: the lift is
        // TAPERED (less than the full inland lift), so the chain ends
        // at the coast instead of stepping into the water.
        const f32 shallow = spec.seaLevel + 4.0f;
        f32 coastPeak = 0.0f;
        f32 inlandPeak = 0.0f;
        for (f32 dx = -1200.0f; dx <= 1200.0f; dx += 40.0f) {
            coastPeak = glm::max(
                coastPeak, shape(ridgeX + dx, along, shallow) - shallow);
            inlandPeak = glm::max(
                inlandPeak, shape(ridgeX + dx, along, inland) - inland);
        }
        CHECK(coastPeak < inlandPeak * 0.75f);
    }

    // Corner continuity: styles are hashed per SEGMENT, so walking
    // ALONG a line through a lattice corner crosses a style junction —
    // the cross-fade must keep the ground continuous (a sea arm closes
    // into a bay, never a channel stopping dead against a wall).
    {
        const f32 lineX = static_cast<f32>(ridgeLine) * spec.mapSize;
        for (const f32 dx : { 0.0f, 600.0f }) {
            f32 previous = shape(lineX + dx, -3000.0f, inland);
            for (f32 zz = -2996.0f; zz <= 3000.0f; zz += 4.0f) {
                const f32 h = shape(lineX + dx, zz, inland);
                CHECK(std::abs(h - previous) < 4.0f);
                previous = h;
            }
        }
    }
}

// ---------------------------------------------------------------------
// The world layer and the v3 controls (docs/PAYSAGE.md §7.5, N2).

TEST_CASE("world layer: continuous across a map line, bounded slopes") {
    // No per-map state anywhere: the floor read 4 m on either side of
    // the x = 8192 line is the same floor, and the province ramp
    // (baseSmooth) stays walkable away from the coast; the plateaus'
    // escarpments on the stepped floor are walls by design, bounded by
    // their geometry (a 120 m step over >= 50 m).
    ProceduralControlParams pc;
    pc.seed = 1337;
    const ProceduralControls controls { pc };
    const WorldLayerParams& wl = controls.params().world;
    f32 worstJump = 0.0f;
    f32 worstGrad = 0.0f;
    f32 worstScarp = 0.0f;
    f32 worstX = 0.0f;
    f32 worstZ = 0.0f;
    for (f32 z = -20000.0f; z <= 28000.0f; z += 250.0f) {
        const ControlSample a = controls.at(8188.0f, z);
        const ControlSample m = controls.at(8192.0f, z);
        const ControlSample b = controls.at(8196.0f, z);
        if (!a.sea && !b.sea) {
            // A seam shows as a kink (second difference); a ramp, even
            // an escarpment's, is linear over 8 m.
            const f32 kink = std::abs(b.base - 2.0f * m.base + a.base);
            if (kink > worstJump) {
                worstJump = kink;
                const ZoneSample zs = zoneSampleAt(
                    controls.params().world, controls.params().zones,
                    8192.0f, z);
                MESSAGE("kink ", kink, " m at z ", z, ": base ", a.base, " ",
                        m.base, " ", b.base, "; corridor ", m.corridor,
                        " scarp ", m.scarp, " borderDist ", zs.borderDist,
                        " wallSteps ", zs.wallSteps, " storey ",
                        zs.storeyHeight, " smooth ", zs.storeyHeightSmooth,
                        " pieceLift ", zs.pieceLift, " archetype ",
                        zs.archetype);
            }
        }
        for (f32 x = -20000.0f; x <= 28000.0f; x += 500.0f) {
            WorldSample w;
            const ControlSample s = controls.at(x, z, w);
            // Inland only: the coastal escarpment of a high province
            // is the erosion's (cliffs, ravines), not a floor.
            if (s.sea || w.continent <
                             controls.params().world.seaThreshold + 0.5f) {
                continue;
            }
            const f32 gx = (worldSampleAt(wl, x + 50.0f, z).baseSmooth -
                            worldSampleAt(wl, x - 50.0f, z).baseSmooth) /
                           100.0f;
            const f32 gz = (worldSampleAt(wl, x, z + 50.0f).baseSmooth -
                            worldSampleAt(wl, x, z - 50.0f).baseSmooth) /
                           100.0f;
            const f32 sx = (controls.at(x + 50.0f, z).base -
                            controls.at(x - 50.0f, z).base) /
                           100.0f;
            const f32 sz = (controls.at(x, z + 50.0f).base -
                            controls.at(x, z - 50.0f).base) /
                           100.0f;
            worstScarp = glm::max(worstScarp, std::hypot(sx, sz));
            if (std::hypot(gx, gz) > worstGrad) {
                worstGrad = std::hypot(gx, gz);
                worstX = x;
                worstZ = z;
            }
        }
    }
    {
        WorldSample w;
        const ControlSample s = controls.at(worstX, worstZ, w);
        const WorldSample e = worldSampleAt(controls.params().world,
                                            worstX + 50.0f, worstZ);
        const WorldSample o = worldSampleAt(controls.params().world,
                                            worstX - 50.0f, worstZ);
        const WorldSample n = worldSampleAt(controls.params().world,
                                            worstX, worstZ + 50.0f);
        const WorldSample m = worldSampleAt(controls.params().world,
                                            worstX, worstZ - 50.0f);
        MESSAGE("worst gradient at (", worstX, ", ", worstZ, "): base ",
                s.base, " etage ", w.etage, " massif ", w.massif,
                " coast ", w.coast, " continent ", w.continent,
                "; per 100 m along x: d(etage) ", e.etage - o.etage,
                " d(massif) ", e.massif - o.massif, " d(continent) ",
                e.continent - o.continent, " d(base) ", e.base - o.base,
                "; along z: d(etage) ", n.etage - m.etage, " d(massif) ",
                n.massif - m.massif, " d(base) ", n.base - m.base);
    }
    MESSAGE("floor jump across x = 8192: ", worstJump,
            " m; worst inland floor gradient: ", worstGrad,
            "; worst stepped-floor gradient (escarpments): ", worstScarp);
    // A seam reads as tens of meters; a smooth riser's own curvature
    // is not one: a wall's riser is the 90 m blend width read on the
    // site distances (twice as steep in world meters) under the border
    // warp's local compression — measured 4.7 m of second difference
    // over 4 m samples on a one-storey wall of the map-line rampart.
    CHECK(worstJump < 6.0f);
    CHECK(worstGrad <= 0.3f); // the province table + the plateau ramp
    CHECK(worstScarp <= 3.0f); // a four-storey wall (240 m) over the 90 m riser
}

TEST_CASE("world layer: the start is land, temperate, off the massifs for any seed") {
    for (const u32 seed :
         { 1u, 7u, 42u, 99u, 1337u, 2024u, 31337u, 65535u }) {
        ProceduralControlParams pc;
        pc.seed = seed;
        const ProceduralControls controls { pc };
        const WorldLayerParams& world = controls.params().world;
        WorldSample w;
        const ControlSample centre =
            controls.at(world.startX, world.startZ, w);
        // Temperate family: the default (0) and its cover variants,
        // heath (4) and dry meadow (5) — at the start's altitude and
        // pulled climate nothing else produces those ids.
        const auto temperateFamily = [](u8 id) {
            return id == 0 || id == 4 || id == 5 || (id >= 6 && id <= 9);
        };
        // The short start ring: land, no massif, temperate at the
        // centre — no flattened meadow, no étage anchor any more (dev
        // decision 2026-10-08).
        CHECK_FALSE(centre.sea);
        CHECK(w.massif < 0.05f);
        CHECK(temperateFamily(centre.biome));
        // The start map's rect: land, temperate, almost everywhere.
        u32 samples = 0, land = 0, temperate = 0;
        for (f32 z = 200.0f; z < 8192.0f; z += 400.0f) {
            for (f32 x = 200.0f; x < 8192.0f; x += 400.0f) {
                const ControlSample s = controls.at(x, z);
                ++samples;
                land += !s.sea;
                temperate += !s.sea && temperateFamily(s.biome);
            }
        }
        CHECK(100 * land >= 50 * samples);
        CHECK(100 * temperate >= 35 * samples);
        // The 6 km disc around it (the low-country ring): land, a
        // coast may show at its edge.
        u32 discSamples = 0, discLand = 0;
        for (f32 dz = -6000.0f; dz <= 6000.0f; dz += 400.0f) {
            for (f32 dx = -6000.0f; dx <= 6000.0f; dx += 400.0f) {
                if (dx * dx + dz * dz > 3.6e7f) {
                    continue;
                }
                ++discSamples;
                discLand +=
                    !controls.at(world.startX + dx, world.startZ + dz).sea;
            }
        }
        CHECK(100 * discLand >= 40 * discSamples);
    }
}

TEST_CASE("world layer: etage distribution over 200 km") {
    // The world is mostly low country with hills, plateaus rarer, the
    // high mountain rare; the sea a real share; massifs a minority.
    ProceduralControlParams pc;
    pc.seed = 1337;
    const ProceduralControls controls { pc };
    u64 samples = 0, sea = 0, low = 0, hills = 0, plateau = 0, high = 0,
        massif = 0;
    const WorldLayerParams& world = controls.params().world;
    const f32 anchored = world.anchorRadius + world.anchorFade;
    for (f32 z = -100000.0f; z <= 100000.0f; z += 1000.0f) {
        for (f32 x = -100000.0f; x <= 100000.0f; x += 1000.0f) {
            // Outside the start anchor: the world's own statistics.
            if (std::hypot(x - world.startX, z - world.startZ) < anchored) {
                continue;
            }
            WorldSample w;
            const ControlSample s = controls.at(x, z, w);
            ++samples;
            if (s.sea) {
                ++sea;
                continue;
            }
            if (s.base < 150.0f) {
                ++low;
            } else if (s.base < 450.0f) {
                ++hills;
            } else if (s.base < 800.0f) {
                ++plateau;
            } else {
                ++high;
            }
            massif += w.massif > 0.5f;
        }
    }
    const f64 land = static_cast<f64>(samples - sea);
    const f64 seaPct =
        100.0 * static_cast<f64>(sea) / static_cast<f64>(samples);
    const f64 lowPct = 100.0 * static_cast<f64>(low) / land;
    const f64 hillsPct = 100.0 * static_cast<f64>(hills) / land;
    const f64 plateauPct = 100.0 * static_cast<f64>(plateau) / land;
    const f64 highPct = 100.0 * static_cast<f64>(high) / land;
    const f64 massifPct = 100.0 * static_cast<f64>(massif) / land;
    MESSAGE("sea ", seaPct, "% of the world; land: < 150 m ", lowPct,
            "%, 150-450 ", hillsPct, "%, 450-800 ", plateauPct,
            "%, >= 800 ", highPct, "%; massif > 0.5: ", massifPct, "%");
    CHECK(seaPct >= 15.0);
    CHECK(seaPct <= 35.0);
    // The zones' storeys lift the land by 0-4 steps: the provinces
    // read a step higher than the étage table alone.
    CHECK(lowPct >= 10.0);
    CHECK(lowPct <= 60.0);
    CHECK(hillsPct >= 15.0);
    CHECK(hillsPct <= 60.0);
    CHECK(plateauPct >= 8.0);
    CHECK(plateauPct <= 30.0);
    CHECK(highPct >= 2.0);
    CHECK(highPct <= 12.0);
    CHECK(massifPct >= 8.0);
    CHECK(massifPct <= 25.0);
}

TEST_CASE("controls v3: derived fields are bounded and consistent") {
    ProceduralControlParams pc;
    pc.seed = 4242;
    const ProceduralControls controls { pc };
    const WorldLayerParams& world = controls.params().world;
    for (f32 z = -40000.0f; z <= 40000.0f; z += 1250.0f) {
        for (f32 x = -40000.0f; x <= 40000.0f; x += 1250.0f) {
            const ControlSample s = controls.at(x, z);
            CHECK(s.hasBase);
            CHECK(s.trunk == 0.0f);
            CHECK(s.trunkDepth == 0.0f);
            CHECK(s.axisStrength == 0.0f);
            CHECK(s.uplift >= 0.0f);
            CHECK(s.uplift <= 1.0f);
            CHECK(s.calm >= s.gentle - 1.0e-6f);
            CHECK(s.hardness >= 0.0f);
            CHECK(s.hardness <= 1.0f);
            // tier <-> base through the etage table.
            CHECK(etageAltitudeFor(world, s.tier) ==
                  doctest::Approx(
                      glm::min(s.base, world.etageAltitude[3]))
                      .epsilon(0.01));
            if (s.sea) {
                CHECK(s.base == 0.0f);
            }
        }
    }
}

TEST_CASE("controls v3: calm is the rule, pieces and massifs the "
          "exception") {
    ProceduralControlParams pc;
    pc.seed = 1337;
    const ProceduralControls controls { pc };
    for (f32 z = -40000.0f; z <= 40000.0f; z += 400.0f) {
        for (f32 x = -40000.0f; x <= 40000.0f; x += 400.0f) {
            WorldSample w;
            const ControlSample s = controls.at(x, z, w);
            if (s.sea) {
                continue;
            }
            // The story texture is socle (calm): only a massif leaves
            // the family here; the designed lifts are checked below.
            if (w.massif > 0.75f && s.gentle < 0.05f) {
                CHECK(s.calm < 0.4f);
            }
        }
    }
    // A POI's own lift leaves the calm family at its summit.
    const auto sites = poiSitesNear(controls.params().world,
                                    controls.params().poi, -8192.0f,
                                    -8192.0f, 16384.0f, 16384.0f);
    u32 checked = 0;
    for (const PoiSite& site : sites) {
        if (site.tier == PoiTier::Petit || site.height < 60.0f) {
            continue;
        }
        if (site.type != PoiType::Summit && site.type != PoiType::Butte &&
            site.type != PoiType::Mesa) {
            continue;
        }
        const ControlSample s = controls.at(site.x, site.z);
        if (s.sea) {
            continue;
        }
        // A walk may climb to the summit (its corridor is calm by
        // contract); off the corridors the summit leaves the family.
        CHECK((s.calm < 0.35f || s.calm <= s.gentle + 1.0e-3f));
        ++checked;
    }
    CHECK(checked >= 10);
}

TEST_CASE("controls v3: landmark summits on every map") {
    // Local maxima of the base lift above 30 m (the pieces of the
    // jittered 2.4 km grid plus the story mountains' ridges), counted
    // per 8 km cell over 64 x 64 km: a lump every kilometer or two,
    // spread (never an empty land map), never a wall of them.
    ProceduralControlParams pc;
    pc.seed = 1337;
    const ProceduralControls controls { pc };
    constexpr f32 kStep = 200.0f;
    constexpr i32 kN = 320; // 64 km
    vector<f32> lift(static_cast<size_t>(kN) * kN);
    u32 landPerCell[8][8] = {};
    for (i32 j = 0; j < kN; ++j) {
        for (i32 i = 0; i < kN; ++i) {
            const ControlSample s =
                controls.at(-32000.0f + static_cast<f32>(i) * kStep,
                            -32000.0f + static_cast<f32>(j) * kStep);
            lift[static_cast<size_t>(j) * kN + i] =
                s.sea ? 0.0f : s.plateau;
            landPerCell[(j * 200) / 8000][(i * 200) / 8000] += !s.sea;
        }
    }
    u32 perCell[8][8] = {};
    u32 peaks = 0;
    for (i32 j = 1; j < kN - 1; ++j) {
        for (i32 i = 1; i < kN - 1; ++i) {
            const f32 v = lift[static_cast<size_t>(j) * kN + i];
            if (v <= 30.0f) {
                continue;
            }
            bool top = true;
            for (i32 dj = -1; dj <= 1 && top; ++dj) {
                for (i32 di = -1; di <= 1; ++di) {
                    if ((di || dj) &&
                        lift[static_cast<size_t>(j + dj) * kN + i +
                             di] >= v) {
                        top = false;
                        break;
                    }
                }
            }
            if (top) {
                ++peaks;
                ++perCell[(j * 200) / 8000][(i * 200) / 8000];
            }
        }
    }
    u32 worst = 0;
    u32 least = 1000; // over the LAND maps (>= 60 % land)
    u32 landMaps = 0;
    for (u32 r = 0; r < 8; ++r) {
        for (u32 c = 0; c < 8; ++c) {
            worst = glm::max(worst, perCell[r][c]);
            if (landPerCell[r][c] * 10 >= 1600 * 6) {
                least = glm::min(least, perCell[r][c]);
                ++landMaps;
            }
        }
    }
    MESSAGE("piece summits > 30 m: ", peaks, " in 64 cells (",
            static_cast<f64>(peaks) / 64.0, " per map), land maps ",
            landMaps, " min ", least, ", any map max ", worst);
    CHECK(static_cast<f64>(peaks) / 64.0 >= 5.0);
    CHECK(static_cast<f64>(peaks) / 64.0 <= 60.0);
    CHECK(landMaps >= 20);
    CHECK(least >= 1); // never an empty land map
    CHECK(worst <= 90);
}

TEST_CASE("the analytic macro is the pointwise synthesis, bounded") {
    // No erosion compression any more: away from the coast the
    // analytic equals the per-texel macro (same landHeight, same
    // recurve); only the shore profile reads a proxy distance.
    ProceduralControlParams pc;
    pc.seed = 1337;
    const ProceduralControls controls { pc };
    MacroParams macro;
    macro.hillChainWavelength = controls.params().rhythm.crestWavelength;
    macro.bedWavelength = controls.params().rhythm.bedWavelength;
    u32 compared = 0;
    for (f32 z = -30000.0f; z <= 30000.0f; z += 1500.0f) {
        for (f32 x = -30000.0f; x <= 30000.0f; x += 1500.0f) {
            WorldSample w;
            const ControlSample s = controls.at(x, z, w);
            if (s.sea || w.coast > 0.0f) {
                continue;
            }
            // A two-texel synthesis at the point (its shore distance
            // is the grid's: far, like the analytic's proxy inland).
            const GridSpec spec { x, z, 16.0f, 2 };
            const MacroResult r =
                synthesizeMacro(controls, spec, macro, pc.seed);
            const f32 a = macroHeightAnalytic(controls, macro, x, z);
            CHECK(std::abs(a - r.height[0]) < 2.0f);
            ++compared;
        }
    }
    CHECK(compared > 50);
}

// Hidden instrument: a transect of the analytic world through a
// point of interest (the shapes the terrain-map PNG shows):
//   meadows-tests "-tc=macro transect diagnostic" -ns
TEST_CASE("macro transect diagnostic" * doctest::skip()) {
    ProceduralControlParams pc;
    pc.seed = 1337;
    const ProceduralControls controls { pc };
    MacroParams macro;
    macro.hillChainWavelength = controls.params().rhythm.crestWavelength;
    macro.bedWavelength = controls.params().rhythm.bedWavelength;
    const f32 z = 1705.0f;
    for (f32 x = -6500.0f; x <= -2300.0f; x += 100.0f) {
        WorldSample w;
        const ControlSample s = controls.at(x, z, w);
        MESSAGE("x ", x, ": h ",
                macroHeightAnalytic(controls, macro, x, z), " base ",
                s.base, " tier ", s.tier, " piece ", s.plateau,
                " reliefScale ", s.reliefScale, " massif ", w.massif,
                " hillRelief ", s.hillRelief, " bed ", s.bedDepth);
    }
    CHECK(true);
}

TEST_CASE("variety at 45 s: an event every 250 m along a walk") {
    // On the controls and the analytic (no bake): along 6 km transects
    // around the start, sampled every 50 m and scored by 250 m windows
    // (~45 s of run), a window is an event when it meets a point of
    // interest, a change of cover (palette id), a regime flip (hill
    // country / massif / plain) or > 25 m of relief — the dev's
    // Skyrim rule. Mean spacing <= 300 m, worst gap <= 750 m.
    ProceduralControlParams pc;
    pc.seed = 1337;
    const ProceduralControls controls { pc };
    const auto& world = controls.params().world;
    MacroParams macro;
    macro.hillChainWavelength = controls.params().rhythm.crestWavelength;
    macro.bedWavelength = controls.params().rhythm.bedWavelength;
    const auto sites = poiSitesNear(world, controls.params().poi, -4000.0f,
                                    -4000.0f, 12192.0f, 12192.0f);
    const auto poiNear = [&](f32 x, f32 z) {
        for (const PoiSite& s : sites) {
            const f32 reach = s.tier == PoiTier::Petit ? 80.0f : 250.0f;
            if (std::abs(s.x - x) < reach && std::abs(s.z - z) < reach) {
                return true;
            }
        }
        return false;
    };
    const auto regimeOf = [&](const ControlSample& s) {
        return s.sea ? 0 : s.hillRelief > 25.0f ? 2 : s.plateau > 60.0f ? 3 : 1;
    };
    f64 gapSum = 0.0;
    u32 gaps = 0, relief = 0, covers = 0, regimes = 0, pois = 0;
    f32 worst = 0.0f;
    for (u32 t = 0; t < 6; ++t) {
        const f32 ang = static_cast<f32>(t) * 0.5235988f;
        const f32 dx = std::cos(ang);
        const f32 dz = std::sin(ang);
        const f32 x0 = world.startX - dx * 3000.0f;
        const f32 z0 = world.startZ - dz * 3000.0f;
        int prevRegime = -1, prevCover = -1;
        bool prevPoi = false;
        f32 lastEvent = 0.0f;
        for (f32 d = 0.0f; d <= 6000.0f; d += 250.0f) {
            f32 lo = 1.0e9f, hi = -1.0e9f;
            bool poi = false;
            int regime = -1, cover = -1;
            for (f32 k = 0.0f; k < 250.0f; k += 50.0f) {
                const f32 x = x0 + dx * (d + k);
                const f32 z = z0 + dz * (d + k);
                const f32 h = macroHeightAnalytic(controls, macro, x, z);
                lo = glm::min(lo, h);
                hi = glm::max(hi, h);
                poi = poi || poiNear(x, z);
                if (regime < 0) {
                    const ControlSample s = controls.at(x, z);
                    regime = regimeOf(s);
                    cover = static_cast<int>(s.biome) * 16 +
                            static_cast<int>(s.character);
                }
            }
            bool event = false;
            if (hi - lo > 25.0f) {
                event = true;
                ++relief;
            }
            if (prevRegime >= 0 && regime != prevRegime) {
                event = true;
                ++regimes;
            }
            if (prevCover >= 0 && cover != prevCover) {
                event = true;
                ++covers;
            }
            if (poi && !prevPoi) {
                event = true;
                ++pois;
            }
            prevRegime = regime;
            prevCover = cover;
            prevPoi = poi;
            if (event && d > 0.0f) {
                const f32 gap = d - lastEvent;
                gapSum += gap;
                ++gaps;
                worst = glm::max(worst, gap);
                lastEvent = d;
            }
        }
    }
    MESSAGE("event windows: ", gaps, " (relief ", relief, ", regime ", regimes,
            ", cover ", covers, ", poi ", pois, "), mean spacing ",
            gaps ? gapSum / gaps : 0.0, " m, worst gap ", worst, " m");
    CHECK(gaps >= 60);
    // 320 on the analytic (the regime selector's flips went with the
    // zones; the walls and gates of Z3 are the next events); the baked
    // instrument (water, POI) is the judge against the 250 m target.
    // 340 on the analytic since the start is the world as drawn (high
    // country: the climate palette flips less); the baked instrument
    // (water, POI, walls) is the judge against the 250 m target.
    CHECK((gaps ? gapSum / gaps : 1.0e9) <= 340.0);
    CHECK(worst <= 1500.0f); // one transect may run a calm reach
}

TEST_CASE("verticality: the plan terraces the country into cliffs") {
    // On the analytic around the start (8 transects of 3 km, 4 m
    // steps): with verticality 1 the steps steeper than 45 deg are at
    // least 2 % of the walk and three times what verticality 0 leaves;
    // a walk's corridor keeps its terrace near zero (passability).
    ProceduralControlParams pc;
    pc.seed = 1337;
    ProceduralControlParams softPc = pc;
    softPc.poi.verticality = 0.0f;
    const ProceduralControls hard { pc };
    const ProceduralControls soft { softPc };
    const auto& world = hard.params().world;
    MacroParams macro;
    macro.hillChainWavelength = hard.params().rhythm.crestWavelength;
    macro.bedWavelength = hard.params().rhythm.bedWavelength;
    const auto steepShare = [&](const ProceduralControls& c) {
        u64 steps = 0, steep = 0;
        for (u32 t = 0; t < 8; ++t) {
            const f32 ang = static_cast<f32>(t) * 0.3926991f;
            const f32 dx = std::cos(ang);
            const f32 dz = std::sin(ang);
            f32 prev = macroHeightAnalytic(c, macro, world.startX - dx * 1500.0f,
                                           world.startZ - dz * 1500.0f);
            for (f32 d = 4.0f; d <= 3000.0f; d += 4.0f) {
                const f32 h = macroHeightAnalytic(
                    c, macro, world.startX - dx * 1500.0f + dx * d,
                    world.startZ - dz * 1500.0f + dz * d);
                ++steps;
                steep += std::abs(h - prev) > 4.0f;
                prev = h;
            }
        }
        return static_cast<f64>(steep) / static_cast<f64>(glm::max<u64>(steps, 1));
    };
    const f64 hardShare = steepShare(hard);
    const f64 softShare = steepShare(soft);
    MESSAGE("steps > 45 deg: verticality 1 ", 100.0 * hardShare,
            " %, verticality 0 ", 100.0 * softShare, " %");
    // Around the start the zones are the meadow (terrace 0.15 x
    // verticality): the cones carry the knob's share, the zone walls
    // and their gates stand at verticality 0 too (Z3) — the knob adds
    // half again, not double.
    CHECK(hardShare >= 0.01);
    CHECK(hardShare >= 1.5 * softShare);
    u32 corridorSamples = 0;
    f32 worstTerrace = 0.0f;
    for (f32 z = world.startZ - 2000.0f; z <= world.startZ + 2000.0f; z += 100.0f) {
        for (f32 x = world.startX - 2000.0f; x <= world.startX + 2000.0f;
             x += 100.0f) {
            const PlanSample ps = planSampleAt(world, hard.params().poi, x, z);
            if (ps.corridor > 0.9f) {
                ++corridorSamples;
                worstTerrace = glm::max(worstTerrace, hard.at(x, z).terrace);
            }
        }
    }
    MESSAGE("corridor samples ", corridorSamples, ", worst terrace ", worstTerrace);
    CHECK(corridorSamples >= 1);
    CHECK(worstTerrace <= 0.15f);
}

TEST_SUITE_END();
