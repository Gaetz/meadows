#include <doctest/doctest.h>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "data/plugins/TomlWriter.hpp"
#include "engine/core/Hash.hpp"
#include "engine/ecs/World.hpp"
#include "game/SaveGame.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/actors/CharacterTick.hpp"
#include "gameplay/combat/Combat.hpp"
#include "gameplay/inventory/Inventory.hpp"
#include "gameplay/save/SaveForms.hpp"
#include "gameplay/save/SaveState.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/Injuries.hpp"
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/StatusBuildup.hpp"
#include "gameplay/stats/Survival.hpp"
#include "world/scene/Components.hpp"
#include "world/worldspace/WorldForms.hpp"

// Actor capture -> save plugin -> TOML -> resolve ->
// apply on a fresh actor = the same actor. The headless proof of
// "a save is an ordinary plugin" (§2.4/§5, contract §6).

namespace {

core::Guid guid(const char* text) {
    return *core::Guid::fromString(text);
}

const core::Guid kRef = guid("40000000-0000-4000-8000-0000000000aa");
const core::Guid kSword = guid("40000000-0000-4000-8000-0000000000bb");
const core::Guid kCoin = guid("40000000-0000-4000-8000-0000000000cc");

// A minimal world plugin: two cells and the actor's reference in cell A.
constexpr const char* kWorldToml = R"([plugin]
id = "40000000-0000-4000-8000-000000000001"
name = "world"

[[records]]
form = "40000000-0000-4000-8000-000000000002"
type = "WorldspaceForm"
new = true
[records.fields]
editorId = "Overworld"
cellSize = 64.0

[[records]]
form = "40000000-0000-4000-8000-000000000003"
type = "CellForm"
new = true
[records.fields]
worldspace = "40000000-0000-4000-8000-000000000002"
gridX = 0
gridY = 0

[[records]]
form = "40000000-0000-4000-8000-000000000004"
type = "CellForm"
new = true
[records.fields]
worldspace = "40000000-0000-4000-8000-000000000002"
gridX = 1
gridY = 0

[[records]]
form = "40000000-0000-4000-8000-0000000000aa"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "40000000-0000-4000-8000-000000000002"
cell = "40000000-0000-4000-8000-000000000003"
position = [4.0, 0.0, 4.0]
)";

data::FormTypeRegistry makeTypes() {
    data::FormTypeRegistry types;
    world::registerWorldFormTypes(types);
    gameplay::registerSaveFormTypes(types);
    return types;
}

// Builds a fully-statted actor entity (the components the demo actors
// carry) with distinctive values.
ecs::Entity makeActor(ecs::World& world, bool battered) {
    ecs::Entity entity = world.create();
    gameplay::CoreAttributes core;
    gameplay::AttributeSet vitals;
    gameplay::Resonance resonance;
    gameplay::Survival survival;
    gameplay::StatusBuildup buildup;
    gameplay::CombatState combat;
    gameplay::Equipment equipment;
    gameplay::Inventory bag;
    gameplay::Injuries injuries;
    if (battered) {
        core.strength = 9.0f;
        vitals.health = 37.5f;
        vitals.maxHealth = 120.0f;
        resonance.amber = -12.0f;
        survival.hunger = 55.0f;
        buildup.poison = 40.0f;
        combat.posture = 18.0f;
        combat.restSeconds = 3.0f;
        equipment.weapon = kSword;
        gameplay::addItem(bag, kSword, 1);
        gameplay::addItem(bag, kCoin, 25);
        injuries.list.push_back({ gameplay::InjuryType::Cut,
                                  gameplay::BodyPart::Arms, 1, 12.5f });
    }
    entity.set<gameplay::CoreAttributes>(core);
    entity.set<gameplay::AttributeSet>(vitals);
    entity.set<gameplay::Resonance>(resonance);
    entity.set<gameplay::Survival>(survival);
    entity.set<gameplay::StatusBuildup>(buildup);
    entity.set<gameplay::CombatState>(combat);
    entity.set<gameplay::Equipment>(equipment);
    entity.set<gameplay::Inventory>(bag);
    entity.set<gameplay::Injuries>(injuries);
    entity.set<gameplay::AbilitySystem>({});
    return entity;
}

} // namespace

TEST_CASE("save state: full actor round-trip through a TOML save plugin") {
    gameplay::GameplayTagRegistry tags;
    tags.registerTag("State.Dead");
    tags.registerTag("Status.Poisoned");

    ecs::World worldA;
    gameplay::registerGameplayComponents(worldA);
    ecs::Entity actorA = makeActor(worldA, /*battered=*/true);
    {
        // One real-time and one game-time durational effect, mid-life.
        auto& system = actorA.get_mut<gameplay::AbilitySystem>();
        gameplay::ActiveEffect realTime;
        realTime.attribute = core::fnv1a("maxEnergy");
        realTime.op = gameplay::ModifierOp::Add;
        realTime.magnitude = 15.0f;
        realTime.remaining = 4.25f;
        realTime.effectId = system.nextEffectId++;
        system.activeEffects.push_back(realTime);
        gameplay::ActiveEffect gameTimePoison;
        gameTimePoison.attribute = core::fnv1a("health");
        gameTimePoison.magnitude = -1.0f;
        gameTimePoison.period = 1.0f;
        gameTimePoison.sinceLastTick = 0.4f;
        gameTimePoison.remaining = 30.0f;
        gameTimePoison.gameTime = true;
        gameTimePoison.grantedTag = *tags.find("Status.Poisoned");
        gameTimePoison.effectId = system.nextEffectId++;
        system.tags.add(gameTimePoison.grantedTag, tags);
        system.activeEffects.push_back(gameTimePoison);
        const auto& set = actorA.get<gameplay::AttributeSet>();
        gameplay::initializeCurrent(system, set);
        gameplay::recomputeCurrent(set, system);
    }

    // Capture -> save plugin -> TOML -> reparse -> resolve on top of the
    // world plugin (the save is just one more layer).
    data::FormTypeRegistry types = makeTypes();
    data::Plugin save;
    save.id = guid("40000000-0000-4000-8000-000000000099");
    save.name = "slot";
    save.records = gameplay::captureActor(actorA, kRef, tags);
    const str toml = data::writePluginToml(save, types);
    const auto reparsedSave = data::parsePluginToml(toml, types, "slot");
    REQUIRE(reparsedSave.has_value());
    const auto worldPlugin = data::parsePluginToml(kWorldToml, types, "world");
    REQUIRE(worldPlugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*worldPlugin, &*reparsedSave }, types, db);

    // Fresh actor (spawn defaults) + applySavedState = the battered one.
    ecs::World worldB;
    gameplay::registerGameplayComponents(worldB);
    ecs::Entity actorB = makeActor(worldB, /*battered=*/false);
    const auto saved = gameplay::savedRecordsFor(db, kRef);
    REQUIRE(saved.stats != nullptr);
    gameplay::applySavedState(actorB, saved, tags);

    // Reflection-driven comparison of every stat component.
    const auto compare = [&](const reflect::TypeInfo& type, const void* a,
                             const void* b) {
        for (const reflect::FieldInfo& field : type.fields) {
            INFO(type.name, ".", field.name);
            CHECK(field.get(a) == field.get(b));
        }
    };
    compare(gameplay::CoreAttributes::staticTypeInfo(),
            &actorA.get<gameplay::CoreAttributes>(),
            &actorB.get<gameplay::CoreAttributes>());
    compare(gameplay::AttributeSet::staticTypeInfo(),
            &actorA.get<gameplay::AttributeSet>(),
            &actorB.get<gameplay::AttributeSet>());
    compare(gameplay::Resonance::staticTypeInfo(),
            &actorA.get<gameplay::Resonance>(),
            &actorB.get<gameplay::Resonance>());
    compare(gameplay::Survival::staticTypeInfo(),
            &actorA.get<gameplay::Survival>(),
            &actorB.get<gameplay::Survival>());
    compare(gameplay::StatusBuildup::staticTypeInfo(),
            &actorA.get<gameplay::StatusBuildup>(),
            &actorB.get<gameplay::StatusBuildup>());
    compare(gameplay::CombatState::staticTypeInfo(),
            &actorA.get<gameplay::CombatState>(),
            &actorB.get<gameplay::CombatState>());
    compare(gameplay::Equipment::staticTypeInfo(),
            &actorA.get<gameplay::Equipment>(),
            &actorB.get<gameplay::Equipment>());

    // Containers.
    const auto& bagB = actorB.get<gameplay::Inventory>();
    CHECK(gameplay::itemCount(bagB, kSword) == 1);
    CHECK(gameplay::itemCount(bagB, kCoin) == 25);
    const auto& injuriesB = actorB.get<gameplay::Injuries>();
    REQUIRE(injuriesB.list.size() == 1);
    CHECK(injuriesB.list[0].severity == 1);
    CHECK(injuriesB.list[0].recoveryHoursRemaining ==
          doctest::Approx(12.5f));

    // Effects: both rows back, currents recomputed from bases (§6).
    const auto& systemB = actorB.get<gameplay::AbilitySystem>();
    REQUIRE(systemB.activeEffects.size() == 2);
    CHECK(systemB.tags.has(*tags.find("Status.Poisoned")));
    CHECK(gameplay::currentValueOf(systemB, core::fnv1a("maxEnergy")) ==
          doctest::Approx(100.0f + 15.0f));
    const auto& systemA = actorA.get<gameplay::AbilitySystem>();
    CHECK(gameplay::currentValueOf(systemB, core::fnv1a("health")) ==
          doctest::Approx(
              gameplay::currentValueOf(systemA, core::fnv1a("health"))));
}

TEST_CASE("save state: a dead actor loads dead (life state re-derived)") {
    gameplay::GameplayTagRegistry tags;
    tags.registerTag("State.Dead");

    ecs::World worldA;
    gameplay::registerGameplayComponents(worldA);
    ecs::Entity dead = makeActor(worldA, true);
    {
        auto& vitals = dead.get_mut<gameplay::AttributeSet>();
        vitals.health = 0.0f;
        auto& system = dead.get_mut<gameplay::AbilitySystem>();
        gameplay::initializeCurrent(system, vitals);
        gameplay::recomputeCurrent(vitals, system);
        gameplay::updateLifeState(system, tags);
        REQUIRE(system.tags.has(*tags.find("State.Dead")));
    }
    data::FormTypeRegistry types = makeTypes();
    data::Plugin save;
    save.records = gameplay::captureActor(dead, kRef, tags);
    data::FormDatabase db;
    data::resolve({ &save }, types, db);

    ecs::World worldB;
    gameplay::registerGameplayComponents(worldB);
    ecs::Entity loaded = makeActor(worldB, false);
    gameplay::applySavedState(loaded, gameplay::savedRecordsFor(db, kRef),
                              tags);
    CHECK(loaded.get<gameplay::AbilitySystem>().tags.has(
        *tags.find("State.Dead")));
}

TEST_CASE("save state: reference diff patches cell moves, actors only for transforms") {
    data::FormTypeRegistry types = makeTypes();
    const auto worldPlugin = data::parsePluginToml(kWorldToml, types, "world");
    REQUIRE(worldPlugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*worldPlugin }, types, db);

    ecs::World world;
    world::registerSceneComponents(world);

    // An actor moved within its cell: position patch, no cell patch.
    ecs::Entity actor = world.create();
    actor.set<world::RefId>(
        { kRef, db.handleOf(guid("40000000-0000-4000-8000-000000000002")),
          db.handleOf(guid("40000000-0000-4000-8000-000000000003")) });
    actor.set<world::Transform>(
        { Vec3 { 9.0f, 0.5f, 4.0f }, Quat { 1, 0, 0, 0 }, Vec3 { 1.0f } });
    actor.add<world::ActorMarker>();
    auto patch = game::captureReference(actor, db);
    REQUIRE(patch.has_value());
    const u32 positionId =
        world::ReferenceForm::staticTypeInfo().findField("position")->id;
    const u32 cellId =
        world::ReferenceForm::staticTypeInfo().findField("cell")->id;
    CHECK(patch->fields.contains(positionId));
    CHECK_FALSE(patch->fields.contains(cellId));
    CHECK_FALSE(patch->creates);

    // Re-homed to the other cell (the follower contract): cell patch.
    actor.get_mut<world::RefId>().cell =
        db.handleOf(guid("40000000-0000-4000-8000-000000000004"));
    patch = game::captureReference(actor, db);
    REQUIRE(patch.has_value());
    CHECK(patch->fields.contains(cellId));

    // Gone persistent (recruited): cell patches to the NULL guid.
    actor.get_mut<world::RefId>().cell = data::FormHandle {};
    patch = game::captureReference(actor, db);
    REQUIRE(patch.has_value());
    CHECK(patch->fields.at(cellId) == reflect::Value { core::Guid {} });

    // A MOVED non-actor: still no patch (transforms are actor-only).
    ecs::Entity crate = world.create();
    crate.set<world::RefId>(
        { kRef, data::FormHandle {},
          db.handleOf(guid("40000000-0000-4000-8000-000000000003")) });
    crate.set<world::Transform>(
        { Vec3 { 999.0f, 5.0f, 4.0f }, Quat { 1, 0, 0, 0 }, Vec3 { 1.0f } });
    CHECK_FALSE(game::captureReference(crate, db).has_value());
}

TEST_CASE("save state: derived maxima survive applySavedState — the HUD is "
          "right on the very first frame") {
    gameplay::GameplayTagRegistry tags;
    tags.registerTag("State.Dead");
    gameplay::DerivedStatRegistry derived;
    gameplay::registerCoreDerivedStats(derived);
    gameplay::StatsTuningForm tuning;
    const gameplay::CharacterTickContext ctx { derived, tags, tuning };

    // A spawned-then-battered actor, captured.
    ecs::World worldA;
    gameplay::registerGameplayComponents(worldA);
    ecs::Entity actorA = makeActor(worldA, /*battered=*/false);
    actorA.set<gameplay::ResonanceDecays>({});
    gameplay::initializeActorStats(actorA, ctx);
    const f32 formulaMax = gameplay::currentValueOf(
        actorA.get<gameplay::AbilitySystem>(), gameplay::attr("maxHealth"));
    // The raw AttributeSet seed differs from the formula: that gap is
    // what a wrong overlay seed would expose on the HUD.
    REQUIRE(formulaMax !=
            doctest::Approx(actorA.get<gameplay::AttributeSet>().maxHealth));
    actorA.get_mut<gameplay::AttributeSet>().health = 37.0f;

    data::FormTypeRegistry types = makeTypes();
    data::Plugin save;
    save.id = guid("40000000-0000-4000-8000-00000000009a");
    save.name = "slot";
    save.records = gameplay::captureActor(actorA, kRef, tags);
    const auto worldPlugin = data::parsePluginToml(kWorldToml, types, "world");
    REQUIRE(worldPlugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*worldPlugin, &save }, types, db);

    // The load path: spawn init (formula maxima + derived cache), THEN the
    // saved bases land on top.
    ecs::World worldB;
    gameplay::registerGameplayComponents(worldB);
    ecs::Entity actorB = makeActor(worldB, /*battered=*/false);
    actorB.set<gameplay::ResonanceDecays>({});
    gameplay::initializeActorStats(actorB, ctx);
    gameplay::applySavedState(actorB, gameplay::savedRecordsFor(db, kRef),
                              tags);

    const auto& system = actorB.get<gameplay::AbilitySystem>();
    // The derived max keeps its FORMULA value right after the load —
    // seeding the overlay from the raw AttributeSet would read the
    // authored seed here until the next character tick.
    CHECK(gameplay::currentValueOf(system, gameplay::attr("maxHealth")) ==
          doctest::Approx(formulaMax));
    // Non-derived currents refresh from the restored bases immediately.
    CHECK(gameplay::currentValueOf(system, gameplay::attr("health")) ==
          doctest::Approx(37.0f));
}
