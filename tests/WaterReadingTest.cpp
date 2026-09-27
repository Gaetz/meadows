#include <doctest/doctest.h>

#include <cmath>
#include <string>

#include "engine/terrain/WaterBodies.hpp"
#include "engine/terrain/WaterQuery.hpp"
#include "engine/terrain/WaterSim.hpp"
#include "world/spirit/WaterReading.hpp"

using namespace world;
using render::terrain::WaterQuery;
using render::terrain::WaterSimSnapshot;
using render::terrain::WaterSimState;

namespace {

f32 bowl(f32 x, f32 z) {
    const f32 dx = (x - 64.0f) / 64.0f;
    const f32 dz = (z - 64.0f) / 64.0f;
    return 500.0f + 30.0f * (dx * dx + dz * dz);
}

// A spring pool in a bowl, simulated then snapshotted (the E1.a fixture).
WaterSimSnapshot springPool() {
    render::terraingen::GridSpec spec;
    spec.texelSize = 2.0f;
    spec.n = 65;
    WaterSimState state;
    render::terrain::initWindow(state, spec, bowl, -1000.0f);
    render::terrain::WaterSimParams params;
    params.rainRate = 0.0f;
    params.evaporationRate = 0.0f;
    params.borderDrainPerSecond = 0.0f;
    params.seaLevel = -1000.0f;
    params.marginCells = 8;
    render::terrain::stepWindow(state, params, { { 64.0f, 64.0f, 3.0f } }, 120);
    WaterSimSnapshot snap;
    render::terrain::extractSnapshot(state, params, snap);
    return snap;
}

} // namespace

TEST_CASE("water reading: a simulated pool reads wet, still, with its exact volume") {
    const WaterSimSnapshot snap = springPool();
    WaterQuery q { &snap, nullptr, -1000.0f };
    WaterReadingInputs in;
    in.query = &q;
    const WaterReading r = readWater(in, 64.0f, 64.0f, bowl(64.0f, 64.0f));
    CHECK(r.water);
    CHECK(r.depth > 0.03f);
    CHECK(r.body == WaterReading::Body::Pool);
    REQUIRE(r.volumeKnown);
    CHECK(r.volumeExact);
    // The pool is the only water: its flood equals the whole snapshot.
    f64 total = 0.0;
    for (const f32 d : snap.depth) {
        total += d;
    }
    CHECK(r.volume == doctest::Approx(total * 4.0).epsilon(0.001));
    CHECK(r.volume > 0.0f);
    CHECK_FALSE(r.nearestFound); // wet: no search
}

TEST_CASE("water reading: a dry aim points to the nearest water") {
    const WaterSimSnapshot snap = springPool();
    WaterQuery q { &snap, nullptr, -1000.0f };
    WaterReadingInputs in;
    in.query = &q;
    // 40 m west of the pool, on the dry slope.
    const WaterReading r = readWater(in, 24.0f, 64.0f, bowl(24.0f, 64.0f));
    CHECK_FALSE(r.water);
    CHECK(r.body == WaterReading::Body::None);
    CHECK_FALSE(r.volumeKnown);
    REQUIRE(r.nearestFound);
    CHECK(r.nearestDir.x > 0.9f); // east, toward the pool
    CHECK(r.nearestDistance > 10.0f);
    CHECK(r.nearestDistance < 45.0f);
    CHECK(std::string(compassCode(r.nearestDir)) == "E");
    // Out of reach: nothing.
    in.searchRadius = 5.0f;
    CHECK_FALSE(readWater(in, 24.0f, 64.0f, bowl(24.0f, 64.0f)).nearestFound);
}

TEST_CASE("water reading: baked lake and river, spirit spring, sea") {
    render::WaterBodies bodies;
    bodies.seaLevel = 0.0f;
    render::LakeSurface lake;
    lake.level = 110.0f;
    lake.minX = 0.0f;
    lake.minZ = 0.0f;
    lake.maxX = 100.0f;
    lake.maxZ = 100.0f; // maskless: the bbox is the basin
    bodies.lakes.push_back(lake);
    render::RiverSurface river;
    river.flowSpeed = 1.5f;
    river.nodes.push_back({ 200.0f, 0.0f, 50.0f, 4.0f });
    river.nodes.push_back({ 200.0f, 100.0f, 49.0f, 4.0f });
    river.minX = 196.0f;
    river.maxX = 204.0f;
    river.minZ = 0.0f;
    river.maxZ = 100.0f;
    bodies.rivers.push_back(river);
    WaterQuery q { nullptr, &bodies, 0.0f };
    WaterReadingInputs in;
    in.query = &q;
    in.ground = [](f32, f32) { return 100.0f; }; // flat floor 10 m under the level

    const WaterReading onLake = readWater(in, 50.0f, 50.0f, 100.0f);
    CHECK(onLake.water);
    CHECK(onLake.body == WaterReading::Body::Lake);
    CHECK(onLake.lakeLevel == 110.0f);
    CHECK(onLake.lakeArea == doctest::Approx(10000.0f));
    REQUIRE(onLake.volumeKnown);
    CHECK_FALSE(onLake.volumeExact);
    CHECK(onLake.volume == doctest::Approx(100000.0f).epsilon(0.05));

    const WaterReading onRiver = readWater(in, 201.0f, 50.0f, 48.0f);
    CHECK(onRiver.water);
    CHECK(onRiver.body == WaterReading::Body::River);
    CHECK(onRiver.riverWidth == doctest::Approx(8.0f));
    CHECK(onRiver.riverDischarge > 0.0f);

    // A spirit spring at the aim explains the water before any body.
    SpiritSourceList sources;
    render::terrain::SpiritSource spring;
    spring.kind = render::terrain::SpiritKind::Water;
    spring.x = 50.0f;
    spring.z = 50.0f;
    spring.radius = 4.0f;
    spring.rate = 3.0f;
    spring.remaining = 6.0f;
    const core::Guid ws = *core::Guid::fromString("aa000000-0000-4000-8000-000000000001");
    sources.add(spring, ws);
    in.sources = &sources;
    in.worldspace = ws;
    const WaterReading onSpring = readWater(in, 51.0f, 50.0f, 100.0f);
    CHECK(onSpring.body == WaterReading::Body::Spirit);
    CHECK(onSpring.spiritRemaining == doctest::Approx(6.0f));
    in.sources = nullptr;

    // Under sea level with the sea sheet: the sea, no volume.
    const WaterReading atSea = readWater(in, 500.0f, 500.0f, -3.0f);
    CHECK(atSea.water);
    CHECK(atSea.body == WaterReading::Body::Sea);
    CHECK_FALSE(atSea.volumeKnown);
}

TEST_CASE("water reading: compass words follow x east, -z north") {
    CHECK(std::string(compassCode({ 1.0f, 0.0f })) == "E");
    CHECK(std::string(compassCode({ 0.0f, -1.0f })) == "N");
    CHECK(std::string(compassCode({ -1.0f, 0.0f })) == "W");
    CHECK(std::string(compassCode({ 0.0f, 1.0f })) == "S");
    CHECK(std::string(compassCode({ 0.7f, -0.7f })) == "NE");
    CHECK(std::string(compassCode({ -0.7f, 0.7f })) == "SW");
}
