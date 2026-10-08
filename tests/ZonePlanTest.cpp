#include <doctest/doctest.h>

#include <cmath>
#include <map>

#include "engine/terrain/generation/MapGrid.hpp"
#include "engine/terrain/generation/PoiPlan.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"
#include "engine/terrain/generation/ZonePlan.hpp"

// The zones (docs/PAYSAGE.md §4 bis, Z2): a mosaic of ~1 km places
// with an archetype, a storey and a grammar; walls between storeys,
// ramps along the walks.

using namespace render::terraingen;

TEST_CASE("zones: deterministic and identical from both sides of a map line") {
    ProceduralControlParams pc;
    pc.seed = 1337;
    const ProceduralControls controls { pc };
    const auto& world = controls.params().world;
    const auto& zones = controls.params().zones;
    for (f32 z = -3000.0f; z <= 12000.0f; z += 333.0f) {
        const ZoneSample a = zoneSampleAt(world, zones, 8192.0f - 2.0f, z);
        const ZoneSample b = zoneSampleAt(world, zones, 8192.0f - 2.0f, z);
        CHECK(a.archetype == b.archetype);
        CHECK(a.storeyHeight == b.storeyHeight);
        // The floor across the line: a ramp (even a wall's riser), never
        // a kink — the second difference over 8 m stays within a smooth
        // riser's own curvature (a four-storey step over the 90 m blend
        // width: ~2.8 m), far from a seam's tens of meters.
        const f32 h0 = controls.at(8192.0f - 4.0f, z).base;
        const f32 h1 = controls.at(8192.0f, z).base;
        const f32 h2 = controls.at(8192.0f + 4.0f, z).base;
        CHECK(std::abs(h2 - 2.0f * h1 + h0) < 3.0f);
    }
}

TEST_CASE("zones: the rampart — no storey step down toward a Ridges map line") {
    // Along a Ridges segment of the line z = 8192 (map (0,0) | (0,1)),
    // the row the line crosses is never below the rows beside it. With
    // the rule off, the trend steps freely: the violation rate is the
    // control.
    ProceduralControlParams pc;
    pc.seed = 1337;
    const ProceduralControls controls { pc };
    const auto& world = controls.params().world;
    ZoneParams on = controls.params().zones;
    on.mapSize = 8192.0f;
    ZoneParams off = on;
    off.mapSize = 0.0f;
    const auto violations = [&](const ZoneParams& zones) {
        u32 n = 0, bad = 0;
        for (i32 cell = -2; cell <= 2; ++cell) {
            if (mapBorderSegmentStyle(world, 8192.0f, 1, cell, false) !=
                MapEdgeStyle::Ridges) {
                continue;
            }
            for (f32 x = static_cast<f32>(cell) * 8192.0f + 64.0f;
                 x < static_cast<f32>(cell + 1) * 8192.0f; x += 64.0f) {
                const i32 band = zoneSampleAt(world, zones, x, 8192.0f).storey;
                const i32 south =
                    zoneSampleAt(world, zones, x, 8192.0f - 450.0f).storey;
                const i32 north =
                    zoneSampleAt(world, zones, x, 8192.0f + 450.0f).storey;
                ++n;
                if (band < south || band < north) {
                    ++bad;
                }
            }
        }
        REQUIRE(n > 100);
        return static_cast<f32>(bad) / static_cast<f32>(n);
    };
    const f32 withRule = violations(on);
    const f32 without = violations(off);
    MESSAGE("rampart violations: with rule " << withRule << ", without "
                                             << without);
    // A thin row (jitter + warp) can put a 450 m probe in the band
    // itself or two rows over: a few percent, never the free trend's.
    CHECK(withRule < 0.1f);
    CHECK(withRule < without);
}

TEST_CASE("zones: gates through the walls — ramped, scarp off, typed by the border") {
    // On a 6 km square: gate samples exist, each lies on a border
    // with a step (or a ridge), and the control sample there is a
    // ramp (terrace and scarp near zero).
    ProceduralControlParams pc;
    pc.seed = 1337;
    const ProceduralControls controls { pc };
    const auto& world = controls.params().world;
    const auto& zones = controls.params().zones;
    u32 gateSamples = 0, wallSamples = 0, offBorder = 0;
    f32 worstScarp = 0.0f, worstTerrace = 0.0f;
    for (f32 z = 1000.0f; z <= 7000.0f; z += 32.0f) {
        for (f32 x = 1000.0f; x <= 7000.0f; x += 32.0f) {
            const ZoneSample zs = zoneSampleAt(world, zones, x, z);
            if (zs.wall > 0.5f && zs.gate < 0.5f) {
                ++wallSamples;
            }
            if (zs.gate < 0.9f) {
                continue;
            }
            ++gateSamples;
            if (zs.wallSteps < 1.0f && zs.borderStyle != 3) {
                ++offBorder;
            }
            const ControlSample s = controls.at(x, z);
            worstScarp = glm::max(worstScarp, s.scarp);
            worstTerrace = glm::max(worstTerrace, s.terrace);
        }
    }
    MESSAGE("gate samples ", gateSamples, " (off a border ", offBorder,
            "), wall samples ", wallSamples, ", worst scarp ", worstScarp,
            ", worst terrace ", worstTerrace);
    CHECK(gateSamples >= 100);
    CHECK(wallSamples >= 100);
    CHECK(offBorder * 20 <= gateSamples);
    CHECK(worstScarp <= 0.15f);
    CHECK(worstTerrace <= 0.2f);
}

TEST_CASE("zones: every archetype draws somewhere, none everywhere") {
    ProceduralControlParams pc;
    pc.seed = 1337;
    const ProceduralControls controls { pc };
    const auto& world = controls.params().world;
    const auto& zones = controls.params().zones;
    const auto& table = zoneArchetypesOf(zones);
    std::map<u32, u32> counts;
    u32 land = 0;
    for (f32 z = -12000.0f; z <= 20000.0f; z += 250.0f) {
        for (f32 x = -12000.0f; x <= 20000.0f; x += 250.0f) {
            const WorldSample w = worldSampleAt(world, x, z);
            if (w.sea) {
                continue;
            }
            ++land;
            ++counts[zoneArchetypeAt(world, zones, x, z)];
        }
    }
    u32 present = 0;
    for (const auto& [id, n] : counts) {
        const f64 share = 100.0 * n / glm::max(land, 1u);
        MESSAGE(table[id].name, " ", share, " %");
        present += share >= 1.0;
        CHECK(share <= 60.0);
    }
    CHECK(present >= 6);
}

TEST_CASE("zones: height zones within a short walk, walls marked, corridors ramped") {
    // On the start map past the meadow: 2 km discs span at least one
    // storey (p90 - p10 of the floor >= 100 m) for most discs; the
    // walls carry scarp; a walk's corridor keeps terrace near zero.
    ProceduralControlParams pc;
    pc.seed = 1337;
    const ProceduralControls controls { pc };
    const auto& world = controls.params().world;
    const auto& zones = controls.params().zones;
    u32 discs = 0, zoned = 0, samples = 0, walls = 0;
    for (f32 cz = 1000.0f; cz <= 7200.0f; cz += 1000.0f) {
        for (f32 cx = 1000.0f; cx <= 7200.0f; cx += 1000.0f) {
            if (std::hypot(cx - world.startX, cz - world.startZ) <
                world.startRadius + zones.startGap) {
                continue;
            }
            vector<f32> bases;
            for (f32 dz = -2000.0f; dz <= 2000.0f; dz += 100.0f) {
                for (f32 dx = -2000.0f; dx <= 2000.0f; dx += 100.0f) {
                    if (dx * dx + dz * dz > 4.0e6f) {
                        continue;
                    }
                    const ControlSample s = controls.at(cx + dx, cz + dz);
                    if (s.sea) {
                        continue;
                    }
                    bases.push_back(s.base);
                    ++samples;
                    walls += s.scarp > 0.5f;
                }
            }
            if (bases.size() < 100) {
                continue;
            }
            std::sort(bases.begin(), bases.end());
            const f32 span = bases[bases.size() * 9 / 10] - bases[bases.size() / 10];
            ++discs;
            zoned += span >= 100.0f;
        }
    }
    MESSAGE("discs ", discs, ", with >= 100 m of floor span ", zoned,
            "; wall samples ", walls, " of ", samples);
    CHECK(discs >= 10);
    CHECK(100 * zoned >= 60 * discs);
    CHECK(walls * 100 >= samples * 2);
    u32 corridorSamples = 0;
    f32 worstTerrace = 0.0f;
    for (f32 z = world.startZ - 2000.0f; z <= world.startZ + 2000.0f; z += 100.0f) {
        for (f32 x = world.startX - 2000.0f; x <= world.startX + 2000.0f;
             x += 100.0f) {
            const PlanSample ps = planSampleAt(world, controls.params().poi, x, z);
            if (ps.corridor > 0.9f) {
                ++corridorSamples;
                worstTerrace = glm::max(worstTerrace, controls.at(x, z).terrace);
            }
        }
    }
    MESSAGE("corridor samples ", corridorSamples, ", worst terrace ", worstTerrace);
    CHECK(corridorSamples >= 1);
    CHECK(worstTerrace <= 0.15f);
}
