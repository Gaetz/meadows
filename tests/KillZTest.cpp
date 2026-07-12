#include <doctest/doctest.h>

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/StatsTuning.hpp"
#include "world/scene/Components.hpp"
#include "world/scene/KillZ.hpp"
#include "world/worldspace/WorldForms.hpp"

// P0 D2a, extracted headless (audit R6): the kill-z sweep. An actor whose
// feet are below the worldspace's killZ dies through the NORMAL pipeline
// (killOutright -> applyDamage -> life state), never a teleport-back.

namespace {

struct Fixture {
    ecs::World world;
    gameplay::DerivedStatRegistry derived;
    gameplay::GameplayTagRegistry tags;
    gameplay::StatsTuningForm tuning;

    Fixture() {
        world::registerSceneComponents(world);
        gameplay::registerGameplayComponents(world);
        gameplay::registerStatsComponents(world);
        gameplay::registerCoreDerivedStats(derived);
        tags.registerTag("State.Dead");
        tags.registerTag("State.Staggered");
    }

    // The TypedDamageTest sheet: all nine attributes at 20, health 300,
    // currents recomputed — a normal living actor, placed at `position`.
    ecs::Entity makeActor(const Vec3& position) {
        gameplay::CoreAttributes core;
        core.strength = core.constitution = core.grace = 20.0f;
        core.dexterity = core.alacrity = core.perception = 20.0f;
        core.charisma = core.ego = core.insight = 20.0f;
        gameplay::AttributeSet vitals;
        vitals.health = 300.0f;
        gameplay::AbilitySystem system;
        gameplay::recomputeStats(core, vitals, system, derived, nullptr);

        ecs::Entity actor = world.create();
        actor.add<world::ActorMarker>();
        actor.set<world::Transform>({ position });
        actor.set<gameplay::CoreAttributes>(core);
        actor.set<gameplay::AttributeSet>(vitals);
        actor.set<gameplay::AbilitySystem>(system);
        actor.set<gameplay::CombatState>({});
        return actor;
    }

    bool isDead(ecs::Entity actor) const {
        const auto dead = tags.find("State.Dead");
        REQUIRE(dead.has_value());
        return actor.get<gameplay::AbilitySystem>().tags.has(*dead);
    }
};

} // namespace

TEST_CASE("kill-z: an actor below the floor dies through the pipeline") {
    Fixture f;
    const f32 killZ = world::WorldspaceForm {}.killZ; // the Form's default
    CHECK(killZ == doctest::Approx(-200.0f));

    ecs::Entity faller = f.makeActor({ 0.0f, killZ - 100.0f, 0.0f });
    ecs::Entity walker = f.makeActor({ 5.0f, 10.0f, 5.0f });

    world::enforceKillZ(f.world, killZ, f.tags, f.derived, f.tuning);

    // The faller died the NORMAL death: health zeroed, State.Dead tag.
    CHECK(f.isDead(faller));
    CHECK(faller.get<gameplay::AttributeSet>().health ==
          doctest::Approx(0.0f));

    // The actor above the floor is untouched.
    CHECK_FALSE(f.isDead(walker));
    CHECK(walker.get<gameplay::AttributeSet>().health ==
          doctest::Approx(300.0f));
}

TEST_CASE("kill-z: the exempt entity (god mode) survives the sweep") {
    Fixture f;
    ecs::Entity god = f.makeActor({ 0.0f, -500.0f, 0.0f });

    world::enforceKillZ(f.world, -200.0f, f.tags, f.derived, f.tuning, god);
    CHECK_FALSE(f.isDead(god));

    // Without the exemption the same sweep kills it.
    world::enforceKillZ(f.world, -200.0f, f.tags, f.derived, f.tuning);
    CHECK(f.isDead(god));
}

TEST_CASE("kill-z: a corpse below the floor is left alone (no re-kill)") {
    Fixture f;
    ecs::Entity faller = f.makeActor({ 0.0f, -300.0f, 0.0f });

    world::enforceKillZ(f.world, -200.0f, f.tags, f.derived, f.tuning);
    CHECK(f.isDead(faller));

    // A second sweep must skip the corpse (snapshot filter on State.Dead)
    // — same observable state, no second killOutright.
    world::enforceKillZ(f.world, -200.0f, f.tags, f.derived, f.tuning);
    CHECK(f.isDead(faller));
    CHECK(faller.get<gameplay::AttributeSet>().health ==
          doctest::Approx(0.0f));
}
