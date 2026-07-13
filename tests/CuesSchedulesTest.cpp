#include <doctest/doctest.h>

#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/fx/Particles.hpp"
#include "engine/nav/Nav.hpp"
#include "gameplay/ai/AiForms.hpp"
#include "gameplay/ai/ScheduleSystem.hpp"
#include "gameplay/cue/GameplayCues.hpp"
#include "gameplay/interaction/Furniture.hpp"
#include "world/ai/GridNavigator.hpp"

// H7: every runtime skeleton of the group — cues, schedules, furniture
// occupancy, particles, nav stub — headless and deterministic.

TEST_CASE("cue registry: headless no-op, handlers receive, table falls back") {
    gameplay::CueRegistry cues;
    cues.emit({ "Cue.Hit", { 1.0f, 2.0f, 3.0f }, 10.0f }); // no handler: fine
    CHECK(cues.empty());

    u32 received = 0;
    Vec3 lastPosition {};
    const u32 handler = cues.addHandler([&](const gameplay::CueEvent& e) {
        ++received;
        lastPosition = e.position;
    });
    cues.emit({ "Cue.Hit.Slash", { 5.0f, 0.0f, 0.0f }, 12.0f });
    CHECK(received == 1);
    CHECK(lastPosition.x == doctest::Approx(5.0f));
    cues.removeHandler(handler);
    cues.emit({ "Cue.Hit.Slash", {}, 0.0f });
    CHECK(received == 1);

    // Hierarchical fallback through CueForms.
    constexpr const char* kToml = R"(
[plugin]
id = "eeee0000-0000-4000-8000-000000000001"
name = "cues"

[[records]]
form = "eeee0001-0000-4000-8000-000000000001"
type = "CueForm"
new = true
[records.fields]
editorId = "GenericHit"
tag = "Cue.Hit"

[[records]]
form = "eeee0001-0000-4000-8000-000000000002"
type = "CueForm"
new = true
[records.fields]
editorId = "SlashHit"
tag = "Cue.Hit.Slash"
)";
    data::FormTypeRegistry types;
    data::registerVisualFormTypes(types);
    const auto plugin = data::parsePluginToml(kToml, types, "cues");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);

    gameplay::CueTable table;
    table.build(db);
    CHECK(table.find("Cue.Hit.Slash")->editorId == "SlashHit");
    CHECK(table.find("Cue.Hit.Pierce")->editorId == "GenericHit"); // fallback
    CHECK(table.find("Cue.Hit")->editorId == "GenericHit");
    CHECK(table.find("Sound.Whatever") == nullptr);
}

TEST_CASE("schedule evaluation: windows, midnight wrap, mod override") {
    constexpr const char* kBase = R"(
[plugin]
id = "eeee0000-0000-4000-8000-000000000002"
name = "schedule-base"

[[records]]
form = "eeee0002-0000-4000-8000-000000000001"
type = "ScheduleForm"
new = true
[records.fields]
editorId = "Innkeeper"

[[records]]
form = "eeee0002-0000-4000-8000-000000000002"
type = "AiPackageForm"
new = true
[records.fields]
editorId = "Work"
kind = "work"

[[records]]
form = "eeee0002-0000-4000-8000-000000000003"
type = "AiPackageForm"
new = true
[records.fields]
editorId = "Sleep"
kind = "sleep"

[[records]]
form = "eeee0002-0000-4000-8000-000000000004"
type = "ScheduleEntryForm"
new = true
[records.fields]
editorId = "WorkDay"
parent = "eeee0002-0000-4000-8000-000000000001"
startHour = 8.0
endHour = 22.0
package = "eeee0002-0000-4000-8000-000000000002"

[[records]]
form = "eeee0002-0000-4000-8000-000000000005"
type = "ScheduleEntryForm"
new = true
[records.fields]
editorId = "Night"
parent = "eeee0002-0000-4000-8000-000000000001"
startHour = 22.0
endHour = 8.0
package = "eeee0002-0000-4000-8000-000000000003"
)";
    // A mod reroutes the evening: later entry wins over the overlap.
    constexpr const char* kMod = R"(
[plugin]
id = "eeee0000-0000-4000-8000-000000000003"
name = "schedule-mod"

[[records]]
form = "eeee0003-0000-4000-8000-000000000001"
type = "AiPackageForm"
new = true
[records.fields]
editorId = "Tavern"
kind = "wander"

[[records]]
form = "eeee0003-0000-4000-8000-000000000002"
type = "ScheduleEntryForm"
new = true
[records.fields]
editorId = "TavernEvening"
parent = "eeee0002-0000-4000-8000-000000000001"
startHour = 19.0
endHour = 22.0
package = "eeee0003-0000-4000-8000-000000000001"
)";
    data::FormTypeRegistry types;
    gameplay::registerAiFormTypes(types);
    const auto base = data::parsePluginToml(kBase, types, "base");
    const auto mod = data::parsePluginToml(kMod, types, "mod");
    REQUIRE(base.has_value());
    REQUIRE(mod.has_value());
    data::FormDatabase db;
    data::resolve({ &*base, &*mod }, types, db);

    const auto scheduleId =
        *core::Guid::fromString("eeee0002-0000-4000-8000-000000000001");

    const auto morning = gameplay::evaluateSchedule(db, scheduleId, 10.0f);
    REQUIRE(morning.has_value());
    CHECK(morning->package->kind == "work");

    // Midnight wrap: 2 AM falls in [22, 8).
    const auto night = gameplay::evaluateSchedule(db, scheduleId, 2.0f);
    REQUIRE(night.has_value());
    CHECK(night->package->kind == "sleep");

    // The mod's evening entry (loaded later) wins the 19-22h overlap.
    const auto evening = gameplay::evaluateSchedule(db, scheduleId, 20.0f);
    REQUIRE(evening.has_value());
    CHECK(evening->package->kind == "wander");
}

TEST_CASE("furniture occupancy: claim, move, full, release") {
    gameplay::FurnitureOccupancy occupancy;
    const core::Guid bench = core::Guid::generate();

    const auto a = occupancy.claim(bench, 2, /*user=*/1);
    REQUIRE(a.has_value());
    const auto b = occupancy.claim(bench, 2, /*user=*/2);
    REQUIRE(b.has_value());
    CHECK(*a != *b);
    CHECK_FALSE(occupancy.claim(bench, 2, /*user=*/3).has_value()); // full
    CHECK(occupancy.occupantCount(bench) == 2);

    occupancy.release(1);
    CHECK(occupancy.claim(bench, 2, /*user=*/3).has_value());
    CHECK(occupancy.pointOf(2).has_value());
    CHECK_FALSE(occupancy.pointOf(1).has_value());
}

TEST_CASE("particles: deterministic bursts, aging, expiry") {
    fx::ParticleSim sim;
    fx::EmitterParams params;
    params.burst = 10;
    params.lifetime = 0.5f;
    params.lifetimeJitter = 0.0f;
    sim.spawn(params, { 0.0f, 0.0f, 0.0f }, 42);
    CHECK(sim.count() == 10);

    fx::ParticleSim sim2;
    sim2.spawn(params, { 0.0f, 0.0f, 0.0f }, 42);
    // Same seed: identical first particle trajectory.
    Vec3 p1 {};
    Vec3 p2 {};
    sim.forEach([&](const Vec3& p, f32, const Vec4&, bool) {
        if (p1 == Vec3 { 0.0f }) { p1 = p; }
    });
    sim.update(0.1f);
    sim2.update(0.1f);
    bool first = true;
    sim.forEach([&](const Vec3& p, f32, const Vec4&, bool) {
        if (first) { p1 = p; first = false; }
    });
    first = true;
    sim2.forEach([&](const Vec3& p, f32, const Vec4&, bool) {
        if (first) { p2 = p; first = false; }
    });
    CHECK(p1.x == doctest::Approx(p2.x));
    CHECK(p1.y == doctest::Approx(p2.y));

    for (int i = 0; i < 60; ++i) {
        sim.update(0.016f); // ~1 s total > 0.5 s lifetime
    }
    CHECK(sim.count() == 0);
}

TEST_CASE("particles v2: continuous emitters, early stop, and the budget") {
    // A 100/s emitter for 0.5 s: the accumulator trickles exactly.
    fx::ParticleSim sim;
    fx::EmitterParams params;
    params.burst = 0;
    params.rate = 100.0f;
    params.duration = 0.5f;
    params.lifetime = 30.0f;
    params.lifetimeJitter = 0.0f;
    const u32 id = sim.spawn(params, { 0.0f, 0.0f, 0.0f }, 7);
    CHECK(id != 0);
    CHECK(sim.emitterCount() == 1);
    sim.update(0.1f);
    CHECK(sim.count() == 10);
    // The duration gates the SLICE: only 0.4 s of the next 0.5 s emits.
    sim.update(0.5f);
    CHECK(sim.count() == 50);
    CHECK(sim.emitterCount() == 0); // expired and swept
    sim.update(0.1f);
    CHECK(sim.count() == 50);

    // stopEmitter ends the stream early; live particles stay.
    fx::ParticleSim early;
    const u32 stream = early.spawn(params, { 0.0f, 0.0f, 0.0f }, 8);
    early.update(0.1f);
    CHECK(early.count() == 10);
    early.stopEmitter(stream);
    early.update(0.2f);
    CHECK(early.count() == 10);

    // The budget drops spawns, never grows the frame.
    fx::ParticleSim capped;
    capped.setBudget(5);
    fx::EmitterParams big;
    big.burst = 20;
    capped.spawn(big, { 0.0f, 0.0f, 0.0f }, 9);
    CHECK(capped.count() == 5);
}

TEST_CASE("particles v2: spawn shapes scatter position or velocity") {
    // Sphere: every spawn inside the radius, none exactly stacked.
    fx::ParticleSim sim;
    fx::EmitterParams sphere;
    sphere.shape = fx::EmitterShape::Sphere;
    sphere.shapeRadius = 2.0f;
    sphere.burst = 32;
    sphere.velocity = { 0.0f, 0.0f, 0.0f };
    sphere.velocityJitter = 0.0f;
    sim.spawn(sphere, { 0.0f, 0.0f, 0.0f }, 3);
    u32 offCenter = 0;
    sim.forEach([&](const Vec3& p, f32, const Vec4&, bool) {
        CHECK(glm::length(p) <= 2.0f + 1e-4f);
        if (glm::length(p) > 0.05f) {
            ++offCenter;
        }
    });
    CHECK(offCenter > 20);

    // Cone: velocities fan around the axis inside the half-angle.
    fx::ParticleSim coneSim;
    fx::EmitterParams cone;
    cone.shape = fx::EmitterShape::Cone;
    cone.shapeRadius = glm::radians(20.0f);
    cone.burst = 1; // inspect one velocity through its first step
    cone.velocity = { 0.0f, 5.0f, 0.0f };
    cone.velocityJitter = 0.0f;
    cone.gravity = { 0.0f, 0.0f, 0.0f };
    cone.lifetime = 10.0f;
    cone.lifetimeJitter = 0.0f;
    const f32 cosHalf = std::cos(glm::radians(20.0f) + 1e-3f);
    for (u32 seed = 0; seed < 16; ++seed) {
        coneSim.clear();
        coneSim.spawn(cone, { 0.0f, 0.0f, 0.0f }, seed);
        coneSim.update(1.0f); // position = velocity after one second
        coneSim.forEach([&](const Vec3& p, f32, const Vec4&, bool) {
            CHECK(glm::length(p) == doctest::Approx(5.0f).epsilon(0.01));
            CHECK(glm::normalize(p).y >= cosHalf);
        });
    }

    // The additive flag rides each particle to the render batches.
    fx::ParticleSim blend;
    fx::EmitterParams add;
    add.burst = 1;
    add.additive = true;
    blend.spawn(add, { 0.0f, 0.0f, 0.0f }, 1);
    blend.forEach([&](const Vec3&, f32, const Vec4&, bool additive) {
        CHECK(additive);
    });
}

TEST_CASE("grid navigator adapts A* to the nav seam") {
    ai::Grid grid { 8, 8 };
    for (i32 y = 0; y < 7; ++y) {
        grid.setBlocked(4, y, true); // wall with a gap at the top
    }
    world::GridNavigator navigator { grid, 1.0f };
    const nav::PathResult path = navigator.findPath(
        { { 1.5f, 1.5f, 0.0f }, { 6.5f, 1.5f, 0.0f } });
    REQUIRE(path.success);
    CHECK(path.waypoints.size() > 8); // detours around the wall
    CHECK(path.waypoints.front().x == doctest::Approx(1.5f));
    CHECK(path.waypoints.back().x == doctest::Approx(6.5f));

    grid.setBlocked(4, 7, true); // seal the gap
    const nav::PathResult blocked = navigator.findPath(
        { { 1.5f, 1.5f, 0.0f }, { 6.5f, 1.5f, 0.0f } });
    CHECK_FALSE(blocked.success);
}

// 8.10 — the ParticleForm -> fx::EmitterParams mapping (the H7 seam
// filled: engine/fx never sees data::, the runtime maps here).
TEST_CASE("toEmitterParams maps every shared field verbatim") {
    data::ParticleForm form;
    form.burst = 24;
    form.lifetime = 2.5f;
    form.lifetimeJitter = 0.4f;
    form.velocity = Vec3 { 1.0f, 2.0f, 3.0f };
    form.velocityJitter = 0.7f;
    form.gravity = Vec3 { 0.0f, -9.0f, 0.0f };
    form.sizeStart = 0.5f;
    form.sizeEnd = 0.1f;
    form.colorStart = Vec4 { 1.0f, 0.5f, 0.25f, 1.0f };
    form.colorEnd = Vec4 { 0.0f, 0.0f, 0.0f, 0.0f };

    const fx::EmitterParams params = gameplay::toEmitterParams(form);
    CHECK(params.burst == 24);
    CHECK(params.lifetime == doctest::Approx(2.5f));
    CHECK(params.lifetimeJitter == doctest::Approx(0.4f));
    CHECK(params.velocity == Vec3 { 1.0f, 2.0f, 3.0f });
    CHECK(params.velocityJitter == doctest::Approx(0.7f));
    CHECK(params.gravity.y == doctest::Approx(-9.0f));
    CHECK(params.sizeStart == doctest::Approx(0.5f));
    CHECK(params.sizeEnd == doctest::Approx(0.1f));
    CHECK(params.colorStart == Vec4 { 1.0f, 0.5f, 0.25f, 1.0f });
    CHECK(params.colorEnd.w == doctest::Approx(0.0f));

    // The mapped params drive the REAL sim (the preview's loop).
    fx::ParticleSim sim;
    sim.spawn(params, Vec3 { 0.0f }, 7u);
    CHECK(sim.count() == 24);
}

TEST_CASE("schedule interruption edges fire exactly once each way") {
    // E-catalogue leftover (2026-07-13): the pure edge detector behind
    // combat/dialogue overriding the schedule — Interrupted once on the
    // rising edge, Resumed once on the falling edge, silent otherwise.
    bool interrupted = false;
    using gameplay::ScheduleSignal;
    using gameplay::updateInterruption;

    CHECK(updateInterruption(interrupted, false) == ScheduleSignal::None);
    CHECK(updateInterruption(interrupted, true) ==
          ScheduleSignal::Interrupted);
    CHECK(interrupted);
    CHECK(updateInterruption(interrupted, true) == ScheduleSignal::None);
    CHECK(updateInterruption(interrupted, false) == ScheduleSignal::Resumed);
    CHECK_FALSE(interrupted);
    CHECK(updateInterruption(interrupted, false) == ScheduleSignal::None);
    // A one-frame skirmish still produces both edges.
    CHECK(updateInterruption(interrupted, true) ==
          ScheduleSignal::Interrupted);
    CHECK(updateInterruption(interrupted, false) == ScheduleSignal::Resumed);
}
