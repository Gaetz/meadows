#include <doctest/doctest.h>

#include <memory>

#include "data/forms/FormDatabase.hpp"
#include "engine/ecs/World.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/event/EventBus.hpp"
#include "gameplay/save/SaveState.hpp"
#include "gameplay/stats/Skills.hpp"

// Skills-by-use v1: usage events -> skill XP ->
// thresholds = GameplayEffects. All headless: forms + components + bus.

using core::Guid;
using namespace gameplay;

namespace {

const Guid kSkill = *Guid::fromString("ee000000-0000-4000-8000-000000000001");
const Guid kRank1 = *Guid::fromString("ee000000-0000-4000-8000-000000000002");
const Guid kRank2 = *Guid::fromString("ee000000-0000-4000-8000-000000000003");
const Guid kPerk1 = *Guid::fromString("ee000000-0000-4000-8000-000000000004");
const Guid kPerk2 = *Guid::fromString("ee000000-0000-4000-8000-000000000005");
const Guid kAbility = *Guid::fromString("ee000000-0000-4000-8000-000000000006");

struct Fixture {
    data::FormDatabase db;
    GameplayTagRegistry registry;

    Fixture() {
        auto skill = std::make_unique<SkillForm>();
        skill->id = kSkill;
        skill->editorId = "OneHanded";
        skill->xpPerUse = 5.0f;
        db.add(std::move(skill), SkillForm::staticTypeInfo());

        addThreshold(kRank1, 10.0f, kPerk1);
        addThreshold(kRank2, 20.0f, kPerk2);
        addPerk(kPerk1, "maxHealth", 25.0f);
        addPerk(kPerk2, "maxEnergy", 50.0f);

        // The trained ability: maps to the skill, no cost/effect (v1 the
        // mapping is the only new field — APPEND on AbilityForm).
        auto ability = std::make_unique<AbilityForm>();
        ability->id = kAbility;
        ability->editorId = "SlashTraining";
        ability->skill = kSkill;
        db.add(std::move(ability), AbilityForm::staticTypeInfo());
    }

    void addThreshold(const Guid& id, f32 xp, const Guid& effect) {
        auto threshold = std::make_unique<SkillThresholdForm>();
        threshold->id = id;
        threshold->parent = kSkill;
        threshold->xp = xp;
        threshold->effect = effect;
        db.add(std::move(threshold), SkillThresholdForm::staticTypeInfo());
    }

    void addPerk(const Guid& id, const char* attribute, f32 magnitude) {
        auto effect = std::make_unique<EffectForm>();
        effect->id = id;
        effect->attribute = attribute;
        effect->op = "add";
        effect->magnitude = magnitude;
        effect->duration = "instant"; // §6: bakes into BaseValue -> saves free
        db.add(std::move(effect), EffectForm::staticTypeInfo());
    }
};

} // namespace

TEST_CASE("skills: xp accumulates and thresholds apply once, in order") {
    Fixture f;
    AttributeSet set;
    AbilitySystem system;
    initializeCurrent(system, set);
    SkillProgress progress;

    awardSkillUse(f.db, kSkill, progress, set, system, f.registry);
    CHECK(progress.skills[kSkill].xp == doctest::Approx(5.0f));
    CHECK(progress.skills[kSkill].granted == 0);
    CHECK(baseValueOf(set, attr("maxHealth")) == doctest::Approx(100.0f));

    awardSkillUse(f.db, kSkill, progress, set, system, f.registry);
    CHECK(progress.skills[kSkill].granted == 1); // rank 1 at 10 xp
    CHECK(baseValueOf(set, attr("maxHealth")) == doctest::Approx(125.0f));
    CHECK(baseValueOf(set, attr("maxEnergy")) == doctest::Approx(100.0f));

    awardSkillUse(f.db, kSkill, progress, set, system, f.registry);
    awardSkillUse(f.db, kSkill, progress, set, system, f.registry);
    CHECK(progress.skills[kSkill].granted == 2); // rank 2 at 20 xp
    CHECK(baseValueOf(set, attr("maxEnergy")) == doctest::Approx(150.0f));

    // Further uses never re-apply a crossed threshold.
    awardSkillUse(f.db, kSkill, progress, set, system, f.registry);
    CHECK(baseValueOf(set, attr("maxHealth")) == doctest::Approx(125.0f));
    CHECK(baseValueOf(set, attr("maxEnergy")) == doctest::Approx(150.0f));
}

TEST_CASE("skills: unknown skills and missing effects are never fatal (§5)") {
    Fixture f;
    AttributeSet set;
    AbilitySystem system;
    initializeCurrent(system, set);
    SkillProgress progress;

    const Guid unknown =
        *Guid::fromString("ee000000-0000-4000-8000-0000000000ff");
    awardSkillUse(f.db, unknown, progress, set, system, f.registry);
    CHECK(progress.skills.empty()); // no phantom entry for a modless skill
}

TEST_CASE("skills: OnAbilityUsed drives progression through the EventBus") {
    Fixture f;
    ecs::World world;
    ecs::Entity entity = world.create();
    entity.set<AttributeSet>({});
    entity.set<AbilitySystem>({});
    entity.set<SkillProgress>({});
    initializeCurrent(entity.get_mut<AbilitySystem>(),
                      entity.get_mut<AttributeSet>());

    EventBus bus;
    bindSkillProgression(bus, f.db, f.registry);

    const AbilityForm& ability = *f.db.find<AbilityForm>(kAbility);
    AbilityContext ctx { f.db, f.registry };
    ctx.events = &bus;
    ctx.caster = entity;
    for (int i = 0; i < 2; ++i) {
        auto& set = entity.get_mut<AttributeSet>();
        auto& system = entity.get_mut<AbilitySystem>();
        CHECK(tryActivate(ability, set, system, set, system, ctx));
    }
    const auto& progress = entity.get<SkillProgress>();
    REQUIRE(progress.skills.contains(kSkill));
    CHECK(progress.skills.at(kSkill).xp == doctest::Approx(10.0f));
    CHECK(progress.skills.at(kSkill).granted == 1);
    CHECK(baseValueOf(entity.get<AttributeSet>(), attr("maxHealth")) ==
          doctest::Approx(125.0f));
}

TEST_CASE("skills: capture/apply round-trips SkillProgress in the save") {
    Fixture f;
    ecs::World world;
    ecs::Entity entity = world.create();
    entity.set<AttributeSet>({});
    entity.set<AbilitySystem>({});
    entity.set<SkillProgress>({});
    initializeCurrent(entity.get_mut<AbilitySystem>(),
                      entity.get_mut<AttributeSet>());
    for (int i = 0; i < 3; ++i) { // xp 15, rank 1 granted
        awardSkillUse(f.db, kSkill, entity.get_mut<SkillProgress>(),
                      entity.get_mut<AttributeSet>(),
                      entity.get_mut<AbilitySystem>(), f.registry);
    }

    const Guid refGuid =
        *Guid::fromString("ee000000-0000-4000-8000-0000000000aa");
    const vector<data::Record> records =
        captureActor(entity, refGuid, f.registry);

    // Materialize the captured rows the way the pending layer does.
    SavedStatsForm stats;
    vector<SavedSkillForm> skillRows;
    for (const data::Record& record : records) {
        if (record.typeId == SavedStatsForm::staticTypeInfo().id) {
            stats = formFromRecord<SavedStatsForm>(record);
        } else if (record.typeId == SavedSkillForm::staticTypeInfo().id) {
            skillRows.push_back(formFromRecord<SavedSkillForm>(record));
        }
    }
    REQUIRE(skillRows.size() == 1);
    CHECK(skillRows[0].skill == kSkill);
    CHECK(skillRows[0].xp == doctest::Approx(15.0f));
    CHECK(skillRows[0].granted == 1);

    ecs::Entity reloaded = world.create();
    reloaded.set<AttributeSet>({});
    reloaded.set<AbilitySystem>({});
    reloaded.set<SkillProgress>({});
    SavedActorRecords saved;
    saved.stats = &stats;
    for (const SavedSkillForm& row : skillRows) {
        saved.skills.push_back(&row);
    }
    applySavedState(reloaded, saved, f.registry);
    const auto& progress = reloaded.get<SkillProgress>();
    REQUIRE(progress.skills.contains(kSkill));
    CHECK(progress.skills.at(kSkill).xp == doctest::Approx(15.0f));
    CHECK(progress.skills.at(kSkill).granted == 1);
}
