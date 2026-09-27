#include <doctest/doctest.h>

#include <cmath>

#include "world/spirit/SpiritJets.hpp"

using namespace world;
using render::terrain::SpiritKind;

namespace {

const f32 kG = SpiritJetList::kGravity;

f32 flat(f32, f32) { return 0.0f; }

} // namespace

TEST_CASE("spirit jets: a level shot lands at the ballistic range") {
    // 45 degrees from 1.6 m up at 10 m/s: range ~ v^2/g + a little for
    // the launch height.
    const f32 v = 10.0f;
    const Vec3 origin { 0.0f, 1.6f, 0.0f };
    const Vec3 velocity { v * std::cos(0.7853982f), v * std::sin(0.7853982f),
                          0.0f };
    const auto landing = jetLanding(origin, velocity, kG, flat);
    REQUIRE(landing.has_value());
    CHECK(landing->point.y == doctest::Approx(0.0f));
    CHECK(landing->point.z == doctest::Approx(0.0f));
    // Exact flight time from the quadratic, compared to the bisected one.
    const f32 vy = velocity.y;
    const f32 tExact = (vy + std::sqrt(vy * vy + 2.0f * kG * 1.6f)) / kG;
    CHECK(landing->flightSeconds == doctest::Approx(tExact).epsilon(0.01));
    CHECK(landing->point.x ==
          doctest::Approx(velocity.x * tExact).epsilon(0.01));
}

TEST_CASE("spirit jets: rising ground catches the arc early, a void never does") {
    const Vec3 origin { 0.0f, 1.6f, 0.0f };
    const Vec3 velocity { 8.0f, 4.0f, 0.0f };
    // A 3 m wall at x = 5: the stream hits its face/top before x = 5.
    auto wall = [](f32 x, f32) { return x >= 5.0f ? 3.0f : 0.0f; };
    const auto onWall = jetLanding(origin, velocity, kG, wall);
    REQUIRE(onWall.has_value());
    CHECK(onWall->point.x >= 4.9f);
    CHECK(onWall->point.y == doctest::Approx(3.0f));
    const auto onFlat = jetLanding(origin, velocity, kG, flat);
    REQUIRE(onFlat.has_value());
    CHECK(onWall->flightSeconds < onFlat->flightSeconds);
    // A bottomless drop: nothing to land on within the horizon.
    auto chasm = [](f32, f32) { return -10000.0f; };
    CHECK_FALSE(jetLanding(origin, velocity, kG, chasm).has_value());
    // A nozzle under the ground lands where it stands.
    auto high = [](f32, f32) { return 5.0f; };
    const auto buried = jetLanding(origin, velocity, kG, high);
    REQUIRE(buried.has_value());
    CHECK(buried->flightSeconds == 0.0f);
    CHECK(buried->point.x == 0.0f);
}

TEST_CASE("spirit jets: a follower re-aims, feeds one water disc, and expires") {
    SpiritJetList jets;
    SpiritJet jet;
    jet.kind = SpiritKind::Water;
    jet.origin = { 0.0f, 1.6f, 0.0f };
    jet.velocity = { 10.0f, 0.0f, 0.0f };
    jet.rate = 2.0f;
    jet.remaining = 3.0f;
    jet.emitter = 7;
    CHECK(jets.start(jet) == 0);
    jets.resolveLandings(flat);
    REQUIRE(jets.entries()[0].landing.has_value());
    const f32 firstX = jets.entries()[0].landing->point.x;
    CHECK(firstX > 0.0f);

    // Turn the caster around: same speed, opposite way.
    jets.aim({ 0.0f, 1.6f, 0.0f }, { -1.0f, 0.0f, 0.0f });
    jets.resolveLandings(flat);
    CHECK(jets.entries()[0].landing->point.x == doctest::Approx(-firstX));
    CHECK(glm::length(jets.entries()[0].velocity) == doctest::Approx(10.0f));

    vector<render::terraingen::WaterSource> sources;
    jets.appendWaterSources(sources);
    REQUIRE(sources.size() == 1);
    CHECK(sources[0].discharge == 2.0f);
    CHECK(sources[0].x == doctest::Approx(-firstX));

    // A non-follower keeps its aim; a non-water jet feeds no water.
    SpiritJet fixed = jet;
    fixed.followsCaster = false;
    fixed.kind = SpiritKind::Fire;
    fixed.emitter = 8;
    jets.start(fixed);
    jets.aim({ 0.0f, 1.6f, 0.0f }, { 0.0f, 0.0f, 1.0f });
    jets.resolveLandings(flat);
    CHECK(jets.entries()[1].velocity.x == 10.0f);
    sources.clear();
    jets.appendWaterSources(sources);
    CHECK(sources.size() == 1);

    // Lifetimes run in sim seconds; expiry hands the emitter back.
    vector<u32> stopped;
    CHECK_FALSE(jets.tick(1.0f, &stopped));
    CHECK(stopped.empty());
    CHECK(jets.tick(2.5f, &stopped));
    CHECK(jets.empty());
    REQUIRE(stopped.size() == 2);
    CHECK(stopped[0] == 7);
    CHECK(stopped[1] == 8);
}

TEST_CASE("spirit jets: the cap evicts the oldest and clear hands every emitter back") {
    SpiritJetList jets;
    for (u32 i = 1; i <= SpiritJetList::kMaxJets; ++i) {
        SpiritJet jet;
        jet.remaining = 1.0f;
        jet.emitter = i;
        CHECK(jets.start(jet) == 0);
    }
    SpiritJet extra;
    extra.remaining = 1.0f;
    extra.emitter = 99;
    CHECK(jets.start(extra) == 1); // the oldest yields its emitter
    CHECK(jets.entries().size() == SpiritJetList::kMaxJets);
    const vector<u32> handles = jets.clear();
    CHECK(handles.size() == SpiritJetList::kMaxJets);
    CHECK(handles.back() == 99);
    CHECK(jets.empty());
}

TEST_CASE("spirit hold: draws only where the element is, caps at capacity, drops it all") {
    SpiritHold hold;
    hold.rate = 6.0f;
    hold.maxVolume = 60.0f; // 6 m³/s x 10 s
    CHECK(hold.absorb(0.5f, false)); // dry aim: nothing drawn
    CHECK(hold.volume == 0.0f);
    CHECK(hold.absorb(0.5f, true));
    CHECK(hold.volume == doctest::Approx(3.0f));
    for (int i = 0; i < 100; ++i) {
        hold.absorb(0.5f, true);
    }
    CHECK(hold.volume == doctest::Approx(60.0f));
    CHECK_FALSE(hold.absorb(0.5f, true)); // full
    CHECK(hold.dropDischarge(1.0f) == doctest::Approx(60.0f));
    CHECK(hold.dropDischarge(2.0f) == doctest::Approx(30.0f));
    CHECK(hold.dropDischarge(0.0f) == 0.0f);
}

TEST_CASE("spirit jets: spheres launch on cadence, fly their own arc, land and splash") {
    SpiritJetList jets;
    SpiritJet jet;
    jet.kind = SpiritKind::Water;
    jet.origin = { 0.0f, 1.6f, 0.0f };
    jet.velocity = { 10.0f, 5.0f, 0.0f };
    jet.rate = 16.0f;
    jet.remaining = 10.0f;
    jets.start(jet);
    jets.resolveLandings(flat);
    const f32 flight = jets.entries()[0].landing->flightSeconds;
    REQUIRE(flight > 0.5f);

    // Half a second at 0.1 s cadence: five lumps in flight, none landed.
    vector<Vec3> landed;
    for (int i = 0; i < 5; ++i) {
        jets.advanceSpheres(0.1f, 0.1f, kG, &landed);
    }
    CHECK(jets.entries()[0].spheres.size() == 5);
    CHECK(landed.empty());
    // Radius from the carried volume: 16 m³/s x 0.1 s = 1.6 m³ -> ~0.73 m.
    CHECK(jets.entries()[0].spheres[0].radius == doctest::Approx(0.726f).epsilon(0.02));
    // The oldest lump is the furthest along the arc.
    const Vec3 a = jets.entries()[0].spheres[0].at(kG);
    const Vec3 b = jets.entries()[0].spheres[4].at(kG);
    CHECK(a.x > b.x);

    // Re-aim: lumps already flying keep their own velocity.
    jets.aim({ 0.0f, 1.6f, 0.0f }, { -1.0f, 0.0f, 0.0f });
    jets.resolveLandings(flat);
    jets.advanceSpheres(0.1f, 0.1f, kG, &landed);
    CHECK(jets.entries()[0].spheres[0].velocity.x > 0.0f);
    CHECK(jets.entries()[0].spheres.back().velocity.x < 0.0f);

    // Past the flight time every early lump has landed on the ground,
    // each reported once.
    for (int i = 0; i < 40; ++i) {
        jets.advanceSpheres(0.1f, 0.1f, kG, &landed);
    }
    CHECK(landed.size() >= 30);
    for (const Vec3& p : landed) {
        CHECK(p.y == doctest::Approx(0.0f).epsilon(0.05).scale(1.0f));
    }

    // An expired jet launches nothing more but stays until its last lump
    // falls, its emitter handed back at once.
    jets.entriesMut()[0].emitter = 5;
    vector<u32> stopped;
    CHECK(jets.tick(20.0f, &stopped));
    REQUIRE(stopped.size() == 1);
    CHECK(jets.entries().size() == 1);
    vector<render::terraingen::WaterSource> sources;
    jets.appendWaterSources(sources);
    CHECK(sources.empty()); // a spent jet feeds the kernel no more
    const size_t before = jets.entries()[0].spheres.size();
    jets.advanceSpheres(0.1f, 0.1f, kG, &landed);
    CHECK(jets.entries()[0].spheres.size() <= before);
    for (int i = 0; i < 60; ++i) {
        jets.advanceSpheres(0.1f, 0.1f, kG, &landed);
    }
    CHECK(jets.tick(0.0f, &stopped));
    CHECK(jets.empty());
}
