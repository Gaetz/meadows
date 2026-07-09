#include <doctest/doctest.h>

#include <memory>

#include "data/forms/FormDatabase.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/condition/Condition.hpp"
#include "gameplay/inventory/Inventory.hpp"

using core::Guid;
using namespace gameplay;

namespace {

const Guid kNode = *Guid::fromString("c1000000-0000-4000-8000-000000000001");
const Guid kSword = *Guid::fromString("c1000000-0000-4000-8000-000000000002");

ConditionForm clause(const char* kind) {
    ConditionForm c;
    c.kind = kind;
    return c;
}

} // namespace

TEST_CASE("condition: structured clauses evaluate against the context") {
    GameplayTagRegistry tags;
    tags.registerTag("Status.Blessed");
    AttributeSet attributes;
    AbilitySystem system;
    initializeCurrent(system, attributes);
    system.tags.add(*tags.find("Status.Blessed"), tags);
    Inventory inventory;
    addItem(inventory, kSword, 2);

    EvalContext ctx;
    ctx.abilitySystem = &system;
    ctx.tags = &tags;
    ctx.inventory = &inventory;
    ctx.luaPredicate = [](std::string_view expr) { return expr == "ok"; };

    SUBCASE("HasTag") {
        ConditionForm c = clause("HasTag");
        c.tag = "Status.Blessed";
        CHECK(evaluateClause(c, ctx));
        c.negate = true;
        CHECK_FALSE(evaluateClause(c, ctx));
    }
    SUBCASE("AttributeAtLeast / AtMost") {
        ConditionForm lo = clause("AttributeAtLeast");
        lo.attribute = "health";
        lo.value = 50.0f;
        CHECK(evaluateClause(lo, ctx)); // 100 >= 50
        lo.value = 150.0f;
        CHECK_FALSE(evaluateClause(lo, ctx));

        ConditionForm hi = clause("AttributeAtMost");
        hi.attribute = "energy";
        hi.value = 100.0f;
        CHECK(evaluateClause(hi, ctx)); // 100 <= 100
    }
    SUBCASE("HasItem") {
        ConditionForm c = clause("HasItem");
        c.item = kSword;
        c.value = 2.0f;
        CHECK(evaluateClause(c, ctx));
        c.value = 3.0f;
        CHECK_FALSE(evaluateClause(c, ctx)); // only have 2
    }
    SUBCASE("Lua escape via callback") {
        ConditionForm c = clause("Lua");
        c.lua = "ok";
        CHECK(evaluateClause(c, ctx));
        c.lua = "nope";
        CHECK_FALSE(evaluateClause(c, ctx));
    }
}

TEST_CASE("condition: conditionsPass ANDs all clauses for a node") {
    data::FormDatabase db;
    const auto add = [&](const char* id, const char* kind, const char* attr,
                         f32 value) {
        auto c = std::make_unique<ConditionForm>();
        c->id = *Guid::fromString(id);
        c->parent = kNode;
        c->kind = kind;
        c->attribute = attr;
        c->value = value;
        db.add(std::move(c), ConditionForm::staticTypeInfo());
    };
    add("c1000000-0000-4000-8000-0000000000a1", "AttributeAtLeast", "health", 50.0f);
    add("c1000000-0000-4000-8000-0000000000a2", "AttributeAtMost", "health", 200.0f);

    AttributeSet attributes;
    AbilitySystem system;
    initializeCurrent(system, attributes);
    EvalContext ctx;
    ctx.abilitySystem = &system;

    CHECK(conditionsPass(db, kNode, ctx)); // 50 <= 100 <= 200
    // An ungated node passes.
    CHECK(conditionsPass(db, kSword, ctx));

    // Drive health below the floor → fails.
    setBaseValue(attributes, attr("health"), 10.0f);
    initializeCurrent(system, attributes);
    CHECK_FALSE(conditionsPass(db, kNode, ctx));
}

TEST_CASE("condition: gates ability activation through AbilityContext.eval") {
    const Guid abilityId = *Guid::fromString("c1000000-0000-4000-8000-0000000000b1");

    data::FormDatabase db;
    auto ability = std::make_unique<AbilityForm>();
    ability->id = abilityId;
    db.add(std::move(ability), AbilityForm::staticTypeInfo());

    auto cond = std::make_unique<ConditionForm>();
    cond->id = *Guid::fromString("c1000000-0000-4000-8000-0000000000b2");
    cond->parent = abilityId;
    cond->kind = "AttributeAtLeast";
    cond->attribute = "energy";
    cond->value = 50.0f;
    db.add(std::move(cond), ConditionForm::staticTypeInfo());

    GameplayTagRegistry tags;
    AttributeSet attributes;
    AbilitySystem system;
    initializeCurrent(system, attributes);

    EvalContext eval;
    eval.abilitySystem = &system;
    eval.tags = &tags;

    const AbilityForm& form = *db.find<AbilityForm>(abilityId);
    AbilityContext ctx { db, tags, &eval };

    CHECK(tryActivate(form, attributes, system, attributes, system, ctx)); // energy 100 >= 50

    setBaseValue(attributes, attr("energy"), 10.0f);
    initializeCurrent(system, attributes);
    CHECK_FALSE(tryActivate(form, attributes, system, attributes, system, ctx)); // 10 < 50
}

// 8.9 — the editor's one-line clause reading (shared by the condition
// builder and the dialogue hierarchy).
TEST_CASE("conditionSummary reads every kind, negate included") {
    ConditionForm clause;
    clause.kind = "HasTag";
    clause.tag = "Faction.Hostile";
    CHECK(conditionSummary(clause) == "if tag Faction.Hostile");
    clause.negate = true;
    CHECK(conditionSummary(clause) == "if not tag Faction.Hostile");

    clause = {};
    clause.kind = "AttributeAtLeast";
    clause.attribute = "health";
    clause.value = 50.0f;
    CHECK(conditionSummary(clause) == "if health >= 50");
    clause.kind = "AttributeAtMost";
    CHECK(conditionSummary(clause) == "if health <= 50");

    clause = {};
    clause.kind = "HasItem";
    clause.value = 3.0f;
    CHECK(conditionSummary(clause) == "if has item x3");
    clause.value = 0.0f; // count defaults to at least 1
    CHECK(conditionSummary(clause) == "if has item x1");

    clause = {};
    clause.kind = "Lua";
    clause.lua = "player.gold > 100";
    CHECK(conditionSummary(clause) == "if lua: player.gold > 100");

    clause = {};
    clause.kind = "QuestStage"; // future kind: degrade readably
    CHECK(conditionSummary(clause) == "if QuestStage (?)");
}
