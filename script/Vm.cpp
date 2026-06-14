#include "script/Vm.hpp"

#include <optional>
#include <stdexcept>
#include <variant>

#include <sol/sol.hpp>

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/event/EventBus.hpp"
#include "script/ScriptVars.hpp"

namespace script {

namespace {

// reflect::Value -> Lua. Scalars map directly; a Guid becomes its string; the
// vector/quat kinds are not exposed to scripts yet (return nil).
sol::object valueToLua(sol::this_state ts, const reflect::Value& value) {
    return std::visit(
        [&](auto&& held) -> sol::object {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, core::Guid>) {
                return sol::make_object(ts, held.toString());
            } else if constexpr (std::is_same_v<T, bool> ||
                                 std::is_same_v<T, i32> ||
                                 std::is_same_v<T, u32> ||
                                 std::is_same_v<T, f32> ||
                                 std::is_same_v<T, str>) {
                return sol::make_object(ts, held);
            } else {
                return sol::make_object(ts, sol::nil);
            }
        },
        value);
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
            return sol::make_object(ts, sol::nil);
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
        return sol::make_object(ts, sol::nil);
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
    sol::state lua;
    u64 rngState { 0x9E3779B97F4A7C15ull };
};

Vm::Vm() : impl(std::make_unique<Impl>()) {
    impl->lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string,
                             sol::lib::math);

    // Sandbox (§8): no filesystem, OS, clock, dynamic loading, or
    // non-deterministic RNG. Randomness comes from the engine-seeded rng().
    for (const char* global :
         { "os", "io", "dofile", "loadfile", "load", "require",
           "collectgarbage", "package" }) {
        impl->lua[global] = sol::nil;
    }
    sol::table math = impl->lua["math"];
    math["random"] = sol::nil;
    math["randomseed"] = sol::nil;
    impl->lua.set_function("rng", [this]() { return nextRandom(); });

    impl->lua.new_usertype<ScriptSelf>(
        "ScriptSelf", sol::meta_function::index, &ScriptSelf::index,
        sol::meta_function::new_index, &ScriptSelf::newindex, "addTag",
        &ScriptSelf::addTag, "removeTag", &ScriptSelf::removeTag, "hasTag",
        &ScriptSelf::hasTag);
}

Vm::~Vm() = default;

RunResult Vm::run(const std::string& code, ScriptContext& context) {
    impl->lua["self"] = ScriptSelf { &context };
    const sol::protected_function_result result =
        impl->lua.safe_script(code, sol::script_pass_on_error);
    impl->lua["self"] = sol::nil; // do not retain a dangling context pointer
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
    impl->lua["self"] = sol::nil;
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

void Vm::seedRng(u64 seed) {
    impl->rngState = seed != 0 ? seed : 1;
}

f64 Vm::nextRandom() {
    // xorshift64* — deterministic, seedable; returns [0, 1).
    u64& state = impl->rngState;
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    const u64 result = state * 0x2545F4914F6CDD1Dull;
    return static_cast<f64>(result >> 11) * (1.0 / 9007199254740992.0);
}

} // namespace script
