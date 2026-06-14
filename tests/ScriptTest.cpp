#include <doctest/doctest.h>

#include <variant>

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/event/EventBus.hpp"
#include "script/ScriptVars.hpp"
#include "script/Vm.hpp"

using namespace script;

TEST_CASE("script: ScriptVars read/write through self.x") {
    Vm vm;
    ScriptVars vars;
    ScriptContext ctx;
    ctx.scriptVars = &vars;

    CHECK(vm.run("self.counter = (self.counter or 0) + 1", ctx).ok);
    CHECK(vm.run("self.counter = (self.counter or 0) + 1", ctx).ok);
    REQUIRE(vars.vars.count("counter") == 1);
    CHECK(std::get<f32>(vars.vars.at("counter")) == 2.0f);

    CHECK(vm.run("self.name = 'hero'; self.alive = true", ctx).ok);
    CHECK(std::get<str>(vars.vars.at("name")) == "hero");
    CHECK(std::get<bool>(vars.vars.at("alive")) == true);
}

TEST_CASE("script: attributes are read-only from scripts (§2.9)") {
    Vm vm;
    gameplay::AttributeSet attributes;
    gameplay::AbilitySystem system;
    gameplay::initializeCurrent(system, attributes);
    ScriptVars vars;
    ScriptContext ctx;
    ctx.attributes = &attributes;
    ctx.abilitySystem = &system;
    ctx.scriptVars = &vars;

    CHECK(vm.run("self.saved = self.health", ctx).ok); // read an attribute
    CHECK(std::get<f32>(vars.vars.at("saved")) == 100.0f);

    const RunResult result = vm.run("self.health = 5", ctx); // write must fail
    CHECK_FALSE(result.ok);
    CHECK(gameplay::currentValueOf(system, gameplay::attr("health")) == 100.0f);
}

TEST_CASE("script: addTag / hasTag through self") {
    Vm vm;
    gameplay::AbilitySystem system;
    gameplay::GameplayTagRegistry tags;
    tags.registerTag("Status.Buffed");
    ScriptVars vars;
    ScriptContext ctx;
    ctx.abilitySystem = &system;
    ctx.tags = &tags;
    ctx.scriptVars = &vars;

    CHECK(vm.run("self:addTag('Status.Buffed'); "
                 "self.buffed = self:hasTag('Status.Buffed')",
                 ctx)
              .ok);
    CHECK(system.tags.has(*tags.find("Status.Buffed")));
    CHECK(std::get<bool>(vars.vars.at("buffed")) == true);
}

TEST_CASE("script: deterministic rng; sandbox removes os") {
    Vm vmA;
    vmA.seedRng(42);
    ScriptVars varsA;
    ScriptContext ctxA;
    ctxA.scriptVars = &varsA;
    vmA.run("self.r = rng()", ctxA);
    const f32 first = std::get<f32>(varsA.vars.at("r"));

    Vm vmB;
    vmB.seedRng(42);
    ScriptVars varsB;
    ScriptContext ctxB;
    ctxB.scriptVars = &varsB;
    vmB.run("self.r = rng()", ctxB);
    CHECK(std::get<f32>(varsB.vars.at("r")) == first); // same seed → same draw

    CHECK_FALSE(vmA.run("return os.time()").ok); // os was removed
}

TEST_CASE("script: a Lua handler subscribes to the event bus") {
    Vm vm;
    gameplay::EventBus bus;
    vm.bindEvents(bus);

    REQUIRE(vm.run("hits = 0\n"
                   "events.on('OnHit', function(e) hits = hits + e.value end)")
                .ok);

    bus.dispatch({ gameplay::eventKind("OnHit"), {}, {}, {}, 5.0f, "" });
    bus.dispatch({ gameplay::eventKind("OnHit"), {}, {}, {}, 3.0f, "" });
    bus.dispatch({ gameplay::eventKind("OnDeath"), {}, {}, {}, 99.0f, "" });

    const auto hits = vm.getNumber("hits");
    REQUIRE(hits.has_value());
    CHECK(*hits == 8.0); // 5 + 3; the OnDeath dispatch is ignored
}
