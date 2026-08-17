#include "script/Vm.hpp"

#include <list>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <variant>

#include <sol/sol.hpp>

#include "engine/core/Log.hpp"
#include "engine/core/Rng.hpp"
#include "engine/reflect/Visit.hpp"

#include "data/forms/FormDatabase.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/combat/CombatAi.hpp" // CombatSituation (brain scripts)
#include "gameplay/event/EventBus.hpp"
#include "script/ScriptVars.hpp"

namespace script {

namespace {

// reflect::Value -> Lua. Scalars map directly; a Guid becomes its string; the
// vector/quat kinds are not exposed to scripts yet (return nil).
sol::object valueToLua(sol::this_state ts, const reflect::Value& value) {
    const auto obj = [&](auto&& held) { return sol::make_object(ts, held); };
    const auto nil = [&] { return sol::make_object(ts, sol::lua_nil); };
    // Exhaustive: a new FieldKind must decide its Lua mapping here. Vec/Quat
    // are deliberately not exposed to scripts yet → nil.
    return reflect::visit(value, reflect::overloaded {
        [&](bool b)              -> sol::object { return obj(b); },
        [&](i32 x)               -> sol::object { return obj(x); },
        [&](u32 x)               -> sol::object { return obj(x); },
        [&](f32 x)               -> sol::object { return obj(x); },
        [&](f64 x)               -> sol::object { return obj(x); },
        [&](const str& s)        -> sol::object { return obj(s); },
        [&](const core::Guid& g) -> sol::object { return obj(g.toString()); },
        [&](const Vec2&)         -> sol::object { return nil(); },
        [&](const Vec3&)         -> sol::object { return nil(); },
        [&](const Vec4&)         -> sol::object { return nil(); },
        [&](const Quat&)         -> sol::object { return nil(); },
    });
}

// Lua -> reflect::Value for the types ScriptVars stores. Numbers become f32;
// nil/unsupported types yield nullopt (the caller erases the var).
std::optional<reflect::Value> luaToValue(const sol::object& object) {
    switch (object.get_type()) {
    case sol::type::boolean:
        return reflect::Value { object.as<bool>() };
    case sol::type::number:
        return reflect::Value { static_cast<f32>(object.as<double>()) };
    case sol::type::string:
        return reflect::Value { object.as<std::string>() };
    default:
        return std::nullopt;
    }
}

// The `self` proxy: a usertype whose index/new_index route `self.x` to attribute
// reads (read-only) or ScriptVars, plus tag methods.
struct ScriptSelf {
    ScriptContext* ctx { nullptr };

    static bool isAttribute(const std::string& name) {
        const reflect::FieldInfo* field =
            gameplay::AttributeSet::staticTypeInfo().findField(name);
        return field && field->kind == reflect::FieldKind::F32;
    }

    sol::object index(sol::stack_object key, sol::this_state ts) {
        if (!ctx || key.get_type() != sol::type::string) {
            return sol::make_object(ts, sol::lua_nil);
        }
        const std::string name = key.as<std::string>();
        if (isAttribute(name)) {
            const f32 value =
                ctx->abilitySystem
                    ? gameplay::currentValueOf(*ctx->abilitySystem,
                                               gameplay::attr(name))
                    : 0.0f;
            return sol::make_object(ts, value);
        }
        if (ctx->scriptVars) {
            const auto it = ctx->scriptVars->vars.find(name);
            if (it != ctx->scriptVars->vars.end()) {
                return valueToLua(ts, it->second);
            }
        }
        return sol::make_object(ts, sol::lua_nil);
    }

    void newindex(sol::stack_object key, sol::stack_object value) {
        if (!ctx || key.get_type() != sol::type::string) {
            return;
        }
        const std::string name = key.as<std::string>();
        if (isAttribute(name)) {
            throw std::runtime_error(
                "attribute '" + name +
                "' is read-only from scripts; change it through a GameplayEffect");
        }
        if (!ctx->scriptVars) {
            return;
        }
        const sol::object object = value;
        if (const auto converted = luaToValue(object)) {
            ctx->scriptVars->vars[name] = *converted;
        } else {
            ctx->scriptVars->vars.erase(name);
        }
    }

    void applyEffect(const std::string& guid) {
        if (!ctx || !ctx->forms || !ctx->attributes || !ctx->abilitySystem ||
            !ctx->tags) {
            return;
        }
        const auto id = core::Guid::fromString(guid);
        if (!id) {
            return;
        }
        if (const gameplay::EffectForm* effect =
                ctx->forms->find<gameplay::EffectForm>(*id)) {
            gameplay::applyEffect(*ctx->attributes, *ctx->abilitySystem, *effect,
                                  *ctx->tags);
        }
    }

    void addTag(const std::string& name) {
        if (ctx && ctx->tags && ctx->abilitySystem) {
            if (const auto tag = ctx->tags->find(name)) {
                ctx->abilitySystem->tags.add(*tag, *ctx->tags);
            }
        }
    }
    void removeTag(const std::string& name) {
        if (ctx && ctx->tags && ctx->abilitySystem) {
            if (const auto tag = ctx->tags->find(name)) {
                ctx->abilitySystem->tags.remove(*tag, *ctx->tags);
            }
        }
    }
    bool hasTag(const std::string& name) {
        if (ctx && ctx->tags && ctx->abilitySystem) {
            if (const auto tag = ctx->tags->find(name)) {
                return ctx->abilitySystem->tags.has(*tag);
            }
        }
        return false;
    }
};

// Re-resolves the per-entity component pointers from the context's entity
// handle. flecs moves component storage on any archetype change (add/remove
// of any component) and frees it on death, so a context held across frames
// (a suspended coroutine) must never trust the pointers captured at
// start. A null handle (id 0: immediate-use contexts, tests) trusts
// the caller's pointers. Returns false when the entity is gone.
bool refreshEntityPointers(ScriptContext& ctx) {
    if (ctx.entity.id() == 0) {
        return true;
    }
    if (!ctx.entity.is_alive()) {
        return false;
    }
    ctx.attributes = ctx.entity.has<gameplay::AttributeSet>()
                         ? &ctx.entity.get_mut<gameplay::AttributeSet>()
                         : nullptr;
    ctx.abilitySystem = ctx.entity.has<gameplay::AbilitySystem>()
                            ? &ctx.entity.get_mut<gameplay::AbilitySystem>()
                            : nullptr;
    ctx.scriptVars = ctx.entity.has<ScriptVars>()
                         ? &ctx.entity.get_mut<ScriptVars>()
                         : nullptr;
    return true;
}

} // namespace

struct Vm::Impl {
    struct Coro {
        sol::thread thread;
        sol::coroutine co;
        ScriptContext self;
        ScriptContext target;
        f32 remaining { 0.0f };
    };

    sol::state lua;
    core::Rng rng; // the shared engine RNG (§8); seeded from the engine's stream
    std::list<Coro> coros; // suspended ability coroutines (stable addresses)
    // Brain scripts: ONE compiled decide function per ActorForm, shared
    // by every instance (stateless, §2.8). An invalid entry caches a
    // compile/runtime failure so the C++ fallback takes over silently.
    std::unordered_map<core::Guid, sol::protected_function> brains;
};

Vm::Vm() : impl(std::make_unique<Impl>()) {
    impl->lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string,
                             sol::lib::math, sol::lib::coroutine);

    // Sandbox (§8): no filesystem, OS, clock, dynamic loading, or
    // non-deterministic RNG. Randomness comes from the engine-seeded rng().
    for (const char* global :
         { "os", "io", "dofile", "loadfile", "load", "require",
           "collectgarbage", "package" }) {
        impl->lua[global] = sol::lua_nil;
    }
    sol::table math = impl->lua["math"];
    math["random"] = sol::lua_nil;
    math["randomseed"] = sol::lua_nil;
    impl->lua.set_function("rng", [this]() { return nextRandom(); });

    impl->lua.new_usertype<ScriptSelf>(
        "ScriptSelf", sol::meta_function::index, &ScriptSelf::index,
        sol::meta_function::new_index, &ScriptSelf::newindex, "applyEffect",
        &ScriptSelf::applyEffect, "addTag", &ScriptSelf::addTag, "removeTag",
        &ScriptSelf::removeTag, "hasTag", &ScriptSelf::hasTag);

    // `wait(t)` suspends the running coroutine for t game-seconds (the scheduler
    // resumes it). Outside a coroutine it is a no-op error, swallowed by run().
    impl->lua.script("function wait(t) return coroutine.yield(t or 0) end");
}

Vm::~Vm() = default;

RunResult Vm::run(const std::string& code, ScriptContext& context) {
    impl->lua["self"] = ScriptSelf { &context };
    const sol::protected_function_result result =
        impl->lua.safe_script(code, sol::script_pass_on_error);
    impl->lua["self"] = sol::lua_nil; // do not retain a dangling context pointer
    if (!result.valid()) {
        const sol::error err = result;
        return { false, err.what() };
    }
    return { true, {} };
}

bool Vm::evalPredicate(const std::string& expr, ScriptContext& context) {
    impl->lua["self"] = ScriptSelf { &context };
    const sol::protected_function_result result =
        impl->lua.safe_script("return (" + expr + ")", sol::script_pass_on_error);
    impl->lua["self"] = sol::lua_nil;
    if (!result.valid()) {
        return false;
    }
    const sol::object value = result;
    if (value.get_type() == sol::type::lua_nil) {
        return false;
    }
    if (value.is<bool>()) {
        return value.as<bool>();
    }
    return true; // any non-nil, non-false value is truthy
}

RunResult Vm::run(const std::string& code) {
    const sol::protected_function_result result =
        impl->lua.safe_script(code, sol::script_pass_on_error);
    if (!result.valid()) {
        const sol::error err = result;
        return { false, err.what() };
    }
    return { true, {} };
}

str Vm::callBrain(const core::Guid& key, const std::string& code,
                  ScriptContext& context,
                  const gameplay::CombatSituation& situation,
                  std::string_view awareState) {
    auto it = impl->brains.find(key);
    if (it == impl->brains.end()) {
        sol::protected_function decide;
        const sol::protected_function_result compiled =
            impl->lua.safe_script(code, sol::script_pass_on_error);
        if (!compiled.valid()) {
            const sol::error err = compiled;
            LOG_WARN("brain {} failed to compile — C++ brain takes over: {}",
                     key.toString(), err.what());
        } else if (const sol::object value = compiled;
                   value.is<sol::protected_function>()) {
            decide = value.as<sol::protected_function>();
        } else {
            LOG_WARN("brain {} must RETURN a decide function — C++ brain "
                     "takes over",
                     key.toString());
        }
        it = impl->brains.emplace(key, std::move(decide)).first;
    }
    if (!it->second.valid()) {
        return {}; // cached failure: the fallback rules
    }
    sol::table sit = impl->lua.create_table();
    sit["distance"] = situation.distance;
    sit["attackRange"] = situation.attackRange;
    sit["preferredRange"] = situation.preferredRange;
    sit["canSee"] = situation.canSee;
    sit["swinging"] = situation.swinging;
    sit["cooldown"] = situation.cooldownSeconds;
    sit["healthFraction"] = situation.healthFraction;
    sit["courage"] = situation.courage;
    sit["aware"] = str { awareState };
    impl->lua["self"] = ScriptSelf { &context };
    const sol::protected_function_result result = it->second(sit);
    impl->lua["self"] = sol::lua_nil;
    if (!result.valid()) {
        const sol::error err = result;
        LOG_WARN("brain {} errored — C++ brain takes over: {}",
                 key.toString(), err.what());
        it->second = sol::protected_function {}; // stop the 4 Hz spam
        return {};
    }
    const sol::object value = result;
    return value.is<std::string>() ? str { value.as<std::string>() }
                                   : str {};
}

void Vm::bindEvents(gameplay::EventBus& bus) {
    sol::table events = impl->lua.create_named_table("events");
    events.set_function(
        "on", [this, &bus](const std::string& name, sol::protected_function fn) {
            bus.subscribe(gameplay::eventKind(name),
                          [this, fn, name](const gameplay::Event& event) {
                              sol::table payload = impl->lua.create_table();
                              payload["source"] = event.source.id();
                              payload["target"] = event.target.id();
                              payload["value"] = event.value;
                              payload["name"] = event.name;
                              payload["tag"] = event.tag.id;
                              const sol::protected_function_result result =
                                  fn(payload);
                              if (!result.valid()) {
                                  // A broken handler must not fail
                                  // silently — nor abort the dispatch.
                                  const sol::error err = result;
                                  LOG_WARN("Lua handler for '{}' failed: {}",
                                           name, err.what());
                              }
                          });
        });
}

std::optional<f64> Vm::getNumber(const std::string& name) {
    const sol::object object = impl->lua[name];
    if (object.get_type() == sol::type::number) {
        return object.as<f64>();
    }
    return std::nullopt;
}

void Vm::startCoroutine(const std::string& code, ScriptContext self,
                        ScriptContext target) {
    impl->coros.emplace_back();
    Impl::Coro& coro = impl->coros.back();
    coro.self = std::move(self);
    coro.target = std::move(target);
    coro.thread = sol::thread::create(impl->lua.lua_state());

    const sol::load_result loaded = coro.thread.state().load(code);
    if (!loaded.valid()) {
        const sol::error err = loaded;
        LOG_WARN("Lua coroutine failed to compile: {}", err.what());
        impl->coros.pop_back();
        return;
    }
    coro.co = sol::coroutine(loaded.get<sol::function>());

    impl->lua["self"] = ScriptSelf { &coro.self };
    impl->lua["target"] = ScriptSelf { &coro.target };
    const sol::protected_function_result result = coro.co();
    impl->lua["self"] = sol::lua_nil;
    impl->lua["target"] = sol::lua_nil;
    if (!result.valid()) {
        const sol::error err = result;
        LOG_WARN("Lua coroutine failed: {}", err.what());
        impl->coros.pop_back();
        return;
    }
    if (coro.co.status() != sol::call_status::yielded) {
        impl->coros.pop_back(); // finished without a wait — not an error
        return;
    }
    coro.remaining = static_cast<f32>(result.get<double>(0));
}

void Vm::tickCoroutines(f32 dt) {
    for (auto it = impl->coros.begin(); it != impl->coros.end();) {
        Impl::Coro& coro = *it;
        coro.remaining -= dt;
        if (coro.remaining > 0.0f) {
            ++it;
            continue;
        }
        if (!refreshEntityPointers(coro.self)) {
            // The acting entity died while suspended: abandon the coroutine —
            // resuming over freed components is UB.
            it = impl->coros.erase(it);
            continue;
        }
        if (!refreshEntityPointers(coro.target)) {
            // A dead target degrades to nil reads (the proxy tolerates null
            // members); the script itself keeps running.
            coro.target.attributes = nullptr;
            coro.target.abilitySystem = nullptr;
            coro.target.scriptVars = nullptr;
        }
        impl->lua["self"] = ScriptSelf { &coro.self };
        impl->lua["target"] = ScriptSelf { &coro.target };
        const sol::protected_function_result result = coro.co();
        impl->lua["self"] = sol::lua_nil;
        impl->lua["target"] = sol::lua_nil;
        if (!result.valid()) {
            const sol::error err = result;
            LOG_WARN("Lua coroutine failed on resume: {}", err.what());
            it = impl->coros.erase(it);
            continue;
        }
        if (coro.co.status() != sol::call_status::yielded) {
            it = impl->coros.erase(it); // finished cleanly
            continue;
        }
        // `remaining` is <= 0 here: the tick overshot the wait by that
        // much. Carrying the deficit into the next wait keeps chained
        // wait(t) at t-cadence instead of rounding every wait up to the
        // tick grid.
        coro.remaining += static_cast<f32>(result.get<double>(0));
        ++it;
    }
}

size_t Vm::pendingCoroutines() const {
    return impl->coros.size();
}

void Vm::seedRng(u64 seed) {
    impl->rng.seed(seed);
}

f64 Vm::nextRandom() {
    return impl->rng.unit(); // [0, 1), via the shared core::Rng (§8)
}

} // namespace script
