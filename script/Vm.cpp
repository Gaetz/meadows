#include "script/Vm.hpp"

#include <list>
#include <optional>
#include <stdexcept>
#include <variant>

#include <sol/sol.hpp>

#include "engine/core/Rng.hpp"
#include "engine/reflect/Visit.hpp"

#include "data/forms/FormDatabase.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ability/GameplayTags.hpp"
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

void Vm::bindEvents(gameplay::EventBus& bus) {
    sol::table events = impl->lua.create_named_table("events");
    events.set_function(
        "on", [this, &bus](const std::string& name, sol::protected_function fn) {
            bus.subscribe(gameplay::eventKind(name),
                          [this, fn](const gameplay::Event& event) {
                              sol::table payload = impl->lua.create_table();
                              payload["source"] = event.source.id();
                              payload["target"] = event.target.id();
                              payload["value"] = event.value;
                              payload["name"] = event.name;
                              payload["tag"] = event.tag.id;
                              (void)fn(payload); // errors swallowed for 4b
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
        impl->coros.pop_back(); // compile error
        return;
    }
    coro.co = sol::coroutine(loaded.get<sol::function>());

    impl->lua["self"] = ScriptSelf { &coro.self };
    impl->lua["target"] = ScriptSelf { &coro.target };
    const sol::protected_function_result result = coro.co();
    impl->lua["self"] = sol::lua_nil;
    impl->lua["target"] = sol::lua_nil;
    if (!result.valid() || coro.co.status() != sol::call_status::yielded) {
        impl->coros.pop_back(); // finished or errored without a wait
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
        impl->lua["self"] = ScriptSelf { &coro.self };
        impl->lua["target"] = ScriptSelf { &coro.target };
        const sol::protected_function_result result = coro.co();
        impl->lua["self"] = sol::lua_nil;
        impl->lua["target"] = sol::lua_nil;
        if (!result.valid() || coro.co.status() != sol::call_status::yielded) {
            it = impl->coros.erase(it); // finished or errored
            continue;
        }
        coro.remaining = static_cast<f32>(result.get<double>(0));
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
