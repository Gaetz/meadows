#include <doctest/doctest.h>

#include <glm/gtc/quaternion.hpp>

#include "gameplay/event/EventBus.hpp"
#include "world/scene/Components.hpp"
#include "world/scene/SpatialIndex.hpp"
#include "world/scene/TriggerSystem.hpp"

// Gameplay volumes: loaded
// TriggerVolumes vs actor positions — enter/leave events on the bus, Lua
// script callback on enter, `once` latch persisted on the component.
// Headless: entities are built by hand (the spawner wiring is covered by
// SpawnerTest / the TOML fixtures elsewhere).

namespace {

struct Fixture {
    ecs::World world;
    gameplay::EventBus bus;
    world::TriggerCallbacks callbacks;
    vector<gameplay::Event> received;

    Fixture() {
        world::registerSceneComponents(world);
        callbacks.events = &bus;
        bus.subscribeAll([this](const gameplay::Event& event) {
            received.push_back(event);
        });
    }

    ecs::Entity makeActor(const Vec3& position) {
        ecs::Entity actor = world.create();
        actor.add<world::ActorMarker>();
        actor.set<world::Transform>({ position });
        return actor;
    }

    ecs::Entity makeTrigger(const Vec3& position, const Vec3& halfExtents,
                            const str& event, bool once = false) {
        ecs::Entity trigger = world.create();
        trigger.add<world::TriggerMarker>();
        trigger.set<world::Transform>({ position });
        world::TriggerVolume volume;
        volume.halfExtents = halfExtents;
        volume.event = event;
        volume.once = once;
        trigger.set<world::TriggerVolume>(volume);
        return trigger;
    }

    void moveActor(ecs::Entity actor, const Vec3& position) {
        actor.get_mut<world::Transform>().position = position;
    }
};

} // namespace

TEST_CASE("trigger volumes: enter and leave fire the data-defined event") {
    Fixture f;
    ecs::Entity actor = f.makeActor({ 50.0f, 0.0f, 0.0f });
    ecs::Entity trigger =
        f.makeTrigger({ 0.0f, 0.0f, 0.0f }, { 2.0f, 2.0f, 2.0f },
                      "OnEnterShrine");

    // Outside: silence, however long we stand there.
    world::updateTriggerVolumes(f.world, f.callbacks);
    world::updateTriggerVolumes(f.world, f.callbacks);
    CHECK(f.received.empty());

    // Step in: ONE event, value 1, actor as source, trigger as target.
    f.moveActor(actor, { 1.0f, 0.5f, -1.0f });
    world::updateTriggerVolumes(f.world, f.callbacks);
    REQUIRE(f.received.size() == 1);
    CHECK(f.received[0].kind == gameplay::eventKind("OnEnterShrine"));
    CHECK(f.received[0].name == "OnEnterShrine");
    CHECK(f.received[0].source == actor);
    CHECK(f.received[0].target == trigger);
    CHECK(f.received[0].value == 1.0f);

    // Linger inside: no re-fire.
    world::updateTriggerVolumes(f.world, f.callbacks);
    CHECK(f.received.size() == 1);

    // Step out: the same event, value 0.
    f.moveActor(actor, { 10.0f, 0.0f, 0.0f });
    world::updateTriggerVolumes(f.world, f.callbacks);
    REQUIRE(f.received.size() == 2);
    CHECK(f.received[1].value == 0.0f);
}

TEST_CASE("trigger volumes: `once` fires a single enter, latched on the "
          "component") {
    Fixture f;
    ecs::Entity actor = f.makeActor({ 50.0f, 0.0f, 0.0f });
    ecs::Entity trigger = f.makeTrigger(
        { 0.0f, 0.0f, 0.0f }, { 2.0f, 2.0f, 2.0f }, "OnAmbush",
        /*once=*/true);

    f.moveActor(actor, { 0.0f, 0.0f, 0.0f });
    world::updateTriggerVolumes(f.world, f.callbacks);
    CHECK(f.received.size() == 1);
    // The latch lives on the REFLECTED component: a save persists it (§5).
    CHECK(trigger.get<world::TriggerVolume>().fired);

    // Leave and re-enter: silence, forever.
    f.moveActor(actor, { 50.0f, 0.0f, 0.0f });
    world::updateTriggerVolumes(f.world, f.callbacks);
    f.moveActor(actor, { 0.0f, 0.0f, 0.0f });
    world::updateTriggerVolumes(f.world, f.callbacks);
    CHECK(f.received.size() == 1);
}

TEST_CASE("trigger volumes: the box is oriented and scaled by the "
          "reference transform") {
    Fixture f;
    ecs::Entity actor = f.makeActor({ 3.5f, 0.0f, 0.0f });
    // A 4x1x1 box rotated 90° around Y: its LONG axis now runs along Z —
    // x = 3.5 is outside (test would pass unrotated).
    ecs::Entity trigger = f.makeTrigger({ 0.0f, 0.0f, 0.0f },
                                        { 4.0f, 1.0f, 1.0f }, "OnZone");
    // NOTE: transform is re-fetched before every write — the first update
    // adds TriggerOccupancy (archetype change) and would dangle a held
    // reference (flecs storage moves — valid in tests too).
    trigger.get_mut<world::Transform>().rotation = glm::angleAxis(
        glm::radians(90.0f), Vec3 { 0.0f, 1.0f, 0.0f });
    world::updateTriggerVolumes(f.world, f.callbacks);
    CHECK(f.received.empty());

    // ...but z = 3.5 is inside the rotated box.
    f.moveActor(actor, { 0.0f, 0.0f, 3.5f });
    world::updateTriggerVolumes(f.world, f.callbacks);
    CHECK(f.received.size() == 1);

    // Scale stretches the extents: x = 5 sits in the box only once the
    // reference is scaled 2x (rotated long axis covers z, short covers x —
    // reset the rotation first).
    f.received.clear();
    f.moveActor(actor, { 5.0f, 0.0f, 0.0f });
    trigger.get_mut<world::Transform>().rotation =
        Quat { 1.0f, 0.0f, 0.0f, 0.0f };
    trigger.get_mut<world::TriggerOccupancy>().inside.clear();
    world::updateTriggerVolumes(f.world, f.callbacks);
    CHECK(f.received.empty()); // 5 > 4: outside at scale 1
    trigger.get_mut<world::Transform>().scale = Vec3 { 2.0f, 2.0f, 2.0f };
    world::updateTriggerVolumes(f.world, f.callbacks);
    CHECK(f.received.size() == 1); // 5 < 8: inside at scale 2
}

TEST_CASE("trigger volumes: the script runs on enter only; an event-less "
          "volume still tracks") {
    Fixture f;
    ecs::Entity actor = f.makeActor({ 50.0f, 0.0f, 0.0f });
    ecs::Entity trigger = f.makeTrigger({ 0.0f, 0.0f, 0.0f },
                                        { 2.0f, 2.0f, 2.0f }, /*event=*/"");
    trigger.get_mut<world::TriggerVolume>().script = "self:addTag('x')";

    u32 scriptRuns = 0;
    f.callbacks.runScript = [&](const str& code, ecs::Entity who,
                                ecs::Entity volume) {
        ++scriptRuns;
        CHECK(code == "self:addTag('x')");
        CHECK(who == actor);
        CHECK(volume == trigger);
    };

    f.moveActor(actor, { 0.0f, 0.0f, 0.0f });
    world::updateTriggerVolumes(f.world, f.callbacks);
    CHECK(scriptRuns == 1);
    CHECK(f.received.empty()); // no event name -> nothing on the bus

    f.moveActor(actor, { 50.0f, 0.0f, 0.0f }); // leave: script does NOT run
    world::updateTriggerVolumes(f.world, f.callbacks);
    CHECK(scriptRuns == 1);

    f.moveActor(actor, { 0.0f, 0.0f, 0.0f }); // not `once`: re-enter re-runs
    world::updateTriggerVolumes(f.world, f.callbacks);
    CHECK(scriptRuns == 2);
}

TEST_CASE("trigger volumes: two actors are tracked independently") {
    Fixture f;
    ecs::Entity first = f.makeActor({ 0.0f, 0.0f, 0.0f }); // starts inside
    ecs::Entity second = f.makeActor({ 50.0f, 0.0f, 0.0f });
    f.makeTrigger({ 0.0f, 0.0f, 0.0f }, { 2.0f, 2.0f, 2.0f }, "OnZone");

    world::updateTriggerVolumes(f.world, f.callbacks);
    REQUIRE(f.received.size() == 1); // first's enter
    CHECK(f.received[0].source == first);

    // Second enters while first stays: one more event, second's.
    f.moveActor(second, { 1.0f, 0.0f, 1.0f });
    world::updateTriggerVolumes(f.world, f.callbacks);
    REQUIRE(f.received.size() == 2);
    CHECK(f.received[1].source == second);

    // First leaves while second stays.
    f.moveActor(first, { 50.0f, 0.0f, 0.0f });
    world::updateTriggerVolumes(f.world, f.callbacks);
    REQUIRE(f.received.size() == 3);
    CHECK(f.received[2].source == first);
    CHECK(f.received[2].value == 0.0f);
}

TEST_CASE("trigger volumes: the spatial-index path matches the full "
          "scan, leave-sweep included") {
    Fixture f;
    ecs::Entity actor = f.makeActor({ 50.0f, 0.0f, 0.0f });
    f.makeTrigger({ 0.0f, 0.0f, 0.0f }, { 2.0f, 2.0f, 2.0f },
                  "OnEnterShrine");
    world::SpatialIndex index;
    const auto tick = [&] {
        index.rebuild(f.world);
        world::updateTriggerVolumes(f.world, f.callbacks, &index);
    };

    // Outside (far outside the bounding radius): silence.
    tick();
    CHECK(f.received.empty());

    // Step in: one enter.
    f.moveActor(actor, { 1.0f, 0.5f, -1.0f });
    tick();
    REQUIRE(f.received.size() == 1);
    CHECK(f.received[0].value == 1.0f);

    // Linger: no re-fire.
    tick();
    CHECK(f.received.size() == 1);

    // Jump FAR away in one tick — outside the query neighborhood
    // entirely: the leave-sweep must still fire the leave.
    f.moveActor(actor, { 200.0f, 0.0f, 200.0f });
    tick();
    REQUIRE(f.received.size() == 2);
    CHECK(f.received[1].value == 0.0f);
    CHECK(f.received[1].source == actor);

    // And a re-entry works after the sweep (occupancy was cleaned).
    f.moveActor(actor, { 0.0f, 0.5f, 0.0f });
    tick();
    REQUIRE(f.received.size() == 3);
    CHECK(f.received[2].value == 1.0f);
}
