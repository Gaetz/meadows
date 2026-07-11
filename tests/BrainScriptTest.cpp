#include <doctest/doctest.h>

#include "gameplay/combat/CombatAi.hpp"
#include "script/Vm.hpp"

// Brain scripts (docs/BOSS-SCRIPTING.md): the Vm compiles a form's
// decide() once, calls it with the flat situation table, and any failure
// hands control back to the C++ brain — silently after the first log.

namespace {

const core::Guid kBrainA =
    *core::Guid::fromString("b0550000-0000-4000-8000-000000000001");
const core::Guid kBrainBroken =
    *core::Guid::fromString("b0550000-0000-4000-8000-000000000002");
const core::Guid kBrainNoFunction =
    *core::Guid::fromString("b0550000-0000-4000-8000-000000000003");
const core::Guid kBrainThrows =
    *core::Guid::fromString("b0550000-0000-4000-8000-000000000004");

} // namespace

TEST_CASE("a brain script decides from the situation table") {
    script::Vm vm;
    script::ScriptContext ctx; // no components: the table is enough here
    gameplay::CombatSituation s;
    s.healthFraction = 0.9f;
    s.canSee = true;
    s.distance = 1.0f;
    s.attackRange = 1.7f;

    const std::string code = R"(
        return function(s)
            if s.healthFraction < 0.5 then return "flee" end
            if s.canSee and s.distance <= s.attackRange then
                return "strike"
            end
            return "approach"
        end
    )";
    CHECK(vm.callBrain(kBrainA, code, ctx, s, "alert") == "strike");

    // Same form key, new situation: the CACHED function answers.
    s.healthFraction = 0.3f;
    CHECK(vm.callBrain(kBrainA, code, ctx, s, "alert") == "flee");
    s.healthFraction = 0.9f;
    s.canSee = false;
    CHECK(vm.callBrain(kBrainA, code, ctx, s, "searching") == "approach");
}

TEST_CASE("broken brains fall back to the C++ brain instead of dying") {
    script::Vm vm;
    script::ScriptContext ctx;
    const gameplay::CombatSituation s;

    // A syntax error: "" every call (compile failure cached).
    CHECK(vm.callBrain(kBrainBroken, "return function(s) nonsense",
                       ctx, s, "alert")
              .empty());
    CHECK(vm.callBrain(kBrainBroken, "return function(s) nonsense",
                       ctx, s, "alert")
              .empty());

    // A script that returns a number instead of a function.
    CHECK(vm.callBrain(kBrainNoFunction, "return 42", ctx, s, "alert")
              .empty());

    // A runtime error inside decide(): "" and the function is dropped.
    CHECK(vm.callBrain(kBrainThrows,
                       "return function(s) error('boom') end", ctx, s,
                       "alert")
              .empty());
    CHECK(vm.callBrain(kBrainThrows,
                       "return function(s) error('boom') end", ctx, s,
                       "alert")
              .empty());

    // And a non-move return maps to nullopt at the parse step.
    CHECK(!gameplay::parseCombatMove("").has_value());
}
