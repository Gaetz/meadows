#include <doctest/doctest.h>

#include <algorithm>

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
#include "gameplay/actors/ActorKit.hpp"
#include "gameplay/actors/CharacterTick.hpp"
#include "gameplay/actors/ScriptVars.hpp"
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

// The actor kit (ActorKit.hpp): the one list the Spawner, the scene and
// the benches attach, and the list the stat sheet is generated from.

TEST_CASE("actor kit: attach wires every kit component, keeps pre-filled "
          "values and is idempotent") {
    ecs::World world;

    ecs::Entity fresh = world.create();
    gameplay::attachActorKit(world, fresh);
    size_t kitSize = 0;
    gameplay::forEachActorKitType([&](auto tag) {
        using T = typename decltype(tag)::type;
        ++kitSize;
        CHECK(fresh.has<T>());
    });
    CHECK(kitSize == 16);
    // Defaults, not garbage: a kit component is default constructed.
    CHECK(fresh.get<gameplay::AttributeSet>().health == 100.0f);
    CHECK(fresh.get<gameplay::Inventory>().items.empty());
    CHECK_FALSE(fresh.get<gameplay::FollowerState>().followerActive);

    // A component seeded BEFORE the kit attaches keeps its values (the
    // Spawner's AttributeSet / AbilitySystem, a bench's loadout).
    ecs::Entity seeded = world.create();
    gameplay::AttributeSet vitals;
    vitals.health = 37.5f;
    seeded.set<gameplay::AttributeSet>(vitals);
    gameplay::Inventory bag;
    gameplay::addItem(bag, kSword, 2);
    seeded.set<gameplay::Inventory>(bag);
    gameplay::attachActorKit(world, seeded);
    CHECK(seeded.get<gameplay::AttributeSet>().health == 37.5f);
    REQUIRE(seeded.get<gameplay::Inventory>().items.size() == 1);
    CHECK(seeded.get<gameplay::Inventory>().items[0].count == 2);
    CHECK(seeded.has<gameplay::CoreAttributes>());
    CHECK(seeded.has<gameplay::SkillProgress>());

    // Attaching again is a no-op on an actor that already has the kit.
    seeded.get_mut<gameplay::Bounty>().bounty = 12.0f;
    gameplay::attachActorKit(world, seeded);
    CHECK(seeded.get<gameplay::Bounty>().bounty == 12.0f);
    CHECK(seeded.get<gameplay::AttributeSet>().health == 37.5f);
}

TEST_CASE("actor kit: the reflection bridge sees exactly the persisted "
          "components of a kit-attached actor") {
    ecs::World world;
    ecs::Entity actor = world.create();
    gameplay::attachActorKit(world, actor);

    vector<str> seen;
    ecs::forEachReflectedComponent(
        actor, [&](const reflect::TypeInfo& type, void* storage) {
            CHECK(storage != nullptr);
            seen.push_back(type.name);
        });
    const auto persisted = gameplay::savedStatsComponentTypes();
    CHECK(seen.size() == persisted.size());
    for (const reflect::TypeInfo* type : persisted) {
        INFO(type->name);
        CHECK(std::find(seen.begin(), seen.end(), type->name) != seen.end());
    }
    // The bridge hands back the live storage, not a copy.
    ecs::forEachReflectedComponent(
        actor, [&](const reflect::TypeInfo& type, void* storage) {
            if (type.id == gameplay::Bounty::staticTypeInfo().id) {
                static_cast<gameplay::Bounty*>(storage)->bounty = 7.0f;
            }
        });
    CHECK(actor.get<gameplay::Bounty>().bounty == 7.0f);
}

TEST_CASE("save state: ScriptVars round-trip as one child record per "
          "variable, sorted by key (§2.8)") {
    gameplay::GameplayTagRegistry tags;
    ecs::World worldA;
    ecs::Entity actorA = worldA.create();
    gameplay::attachActorKit(worldA, actorA);
    gameplay::ScriptVars vars;
    vars.vars["c"] = reflect::Value { true };
    vars.vars["a"] = reflect::Value { 1.0f };
    vars.vars["b"] = reflect::Value { str { "x" } };
    actorA.set<gameplay::ScriptVars>(vars);

    // Capture: the sentinel + one row per variable, in key order, with
    // identities that do not depend on the map's iteration order.
    const vector<data::Record> records =
        gameplay::captureActor(actorA, kRef, tags);
    const u32 rowType = gameplay::SavedScriptVarForm::staticTypeInfo().id;
    vector<str> keys;
    for (const data::Record& record : records) {
        if (record.typeId == rowType) {
            keys.push_back(
                gameplay::formFromRecord<gameplay::SavedScriptVarForm>(record).key);
        }
    }
    CHECK(keys == vector<str> { "a", "b", "c" });
    const vector<data::Record> again =
        gameplay::captureActor(actorA, kRef, tags);
    REQUIRE(again.size() == records.size());
    for (size_t i = 0; i < records.size(); ++i) {
        CHECK(again[i].formId == records[i].formId);
    }

    // Through the full save path (TOML, reparse, resolve) onto a fresh
    // actor that was spawned WITHOUT the component.
    data::FormTypeRegistry types = makeTypes();
    data::Plugin save;
    save.id = guid("40000000-0000-4000-8000-00000000009b");
    save.name = "slot";
    save.records = records;
    const str toml = data::writePluginToml(save, types);
    const auto reparsed = data::parsePluginToml(toml, types, "slot");
    REQUIRE(reparsed.has_value());
    data::FormDatabase db;
    data::resolve({ &*reparsed }, types, db);

    ecs::World worldB;
    ecs::Entity actorB = worldB.create();
    gameplay::attachActorKit(worldB, actorB);
    REQUIRE_FALSE(actorB.has<gameplay::ScriptVars>());
    const auto saved = gameplay::savedRecordsFor(db, kRef);
    REQUIRE(saved.scriptVars.size() == 3);
    gameplay::applySavedState(actorB, saved, tags);
    REQUIRE(actorB.has<gameplay::ScriptVars>());
    const auto& restored = actorB.get<gameplay::ScriptVars>().vars;
    REQUIRE(restored.size() == 3);
    CHECK(restored.at("a") == reflect::Value { 1.0f });
    CHECK(restored.at("b") == reflect::Value { str { "x" } });
    CHECK(restored.at("c") == reflect::Value { true });

    // An actor captured with an EMPTY map reloads empty: the apply clears
    // stale variables (the save is the truth once the sentinel exists).
    actorA.get_mut<gameplay::ScriptVars>().vars.clear();
    data::Plugin emptySave;
    emptySave.records = gameplay::captureActor(actorA, kRef, tags);
    data::FormDatabase emptyDb;
    data::resolve({ &emptySave }, types, emptyDb);
    gameplay::applySavedState(actorB, gameplay::savedRecordsFor(emptyDb, kRef),
                              tags);
    CHECK(actorB.get<gameplay::ScriptVars>().vars.empty());
}

TEST_CASE("save state: a pristine kit actor captures to the sentinel alone") {
    gameplay::GameplayTagRegistry tags;
    ecs::World world;
    ecs::Entity actor = world.create();
    gameplay::attachActorKit(world, actor);

    // Every kit component is present and at its default: the stat sheet
    // diffs to nothing but the child-record link, and no row is emitted.
    const vector<data::Record> records =
        gameplay::captureActor(actor, kRef, tags);
    REQUIRE(records.size() == 1);
    CHECK(records[0].typeId == gameplay::SavedStatsForm::staticTypeInfo().id);
    CHECK(records[0].fields.size() == 1);
    CHECK(records[0].fields.count(core::fnv1a("parent")) == 1);
}
