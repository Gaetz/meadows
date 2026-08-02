#include <doctest/doctest.h>

#include <cmath>

#include "engine/terrain/generation/TerrainGen.hpp"

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
    CHECK(at(rb, 8.0f, 0.0f) ==
          doctest::Approx(params.seaLevel + params.shoreHeight)
              .epsilon(0.15));
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

TEST_CASE("uplift is zero at sea and bounded on land") {
    ProceduralControlParams pc;
    pc.seed = 99;
    const ProceduralControls controls { pc };
    const GridSpec spec { -8192.0f, -8192.0f, 64.0f, 257 };
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
    // Somewhere in 16x16 km a range wants to rise.
    CHECK(maxUplift > 0.2f);
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
            // Ceiling: top tier + relief + every regime extra that can
            // stack (long swell, massif plateau, chain hills).
            CHECK(a <= params.tiers.back().altitude +
                           params.tiers.back().reliefAmplitude +
                           pc.swellHeight + pc.oldMassifHeight +
                           pc.oldMassifHillAmplitude + 1.0f);
            maxSeen = std::max(maxSeen, a);
        }
    }
    CHECK(maxSeen > params.seaLevel); // some land exists
}

TEST_CASE("relief regimes: hill chains, old massifs and young ranges "
          "all exist") {
    ProceduralControlParams params;
    params.seed = 1337;
    const ProceduralControls controls { params };
    u32 hillChains = 0;
    u32 oldMassifs = 0;
    u32 youngRanges = 0;
    // Scan a wide area at coarse steps: the three regimes must all
    // occur on land — the variety contract. Sized against the longest
    // control wave (the swell) so every regime gets several periods.
    for (f32 z = -100000.0f; z <= 100000.0f; z += 2000.0f) {
        for (f32 x = -100000.0f; x <= 100000.0f; x += 2000.0f) {
            const ControlSample s = controls.at(x, z);
            if (s.sea) {
                continue;
            }
            if (s.plateau > 100.0f) {
                ++oldMassifs;
            } else if (s.hillRelief > 20.0f && s.uplift < 0.1f) {
                ++hillChains;
            }
            if (s.uplift > 0.5f) {
                ++youngRanges;
            }
        }
    }
    CHECK(hillChains > 50);
    CHECK(oldMassifs > 50);
    CHECK(youngRanges > 50);

    // The long swell and the passability corridors both exist on land.
    u32 swelled = 0;
    u32 gentle = 0;
    for (f32 z = -100000.0f; z <= 100000.0f; z += 2000.0f) {
        for (f32 x = -100000.0f; x <= 100000.0f; x += 2000.0f) {
            const ControlSample s = controls.at(x, z);
            if (s.sea) {
                continue;
            }
            if (s.plateau > 220.0f) {
                ++swelled; // above what the massif regime alone gives
            }
            if (s.gentle > 0.5f) {
                ++gentle;
            }
        }
    }
    CHECK(swelled > 50);
    CHECK(gentle > 200);

    // Regime extras default to zero for painted/test sources: the
    // legacy macro path is untouched.
    const ControlSample plain;
    CHECK(plain.plateau == 0.0f);
    CHECK(plain.hillRelief == 0.0f);
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
