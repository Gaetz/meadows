#pragma once

#include <memory>
#include <optional>
#include <string>

#include "engine/core/Defines.hpp"

// The Lua scripting VM (Phase 4, §2.8). ONE shared, sandboxed Lua state; scripts
// are stateless modules; `self` is an entity handle (a proxy over the entity's
// components). sol2/Lua are confined to Vm.cpp (pimpl) so they never leak into
// engine headers. Determinism (§8): no os/io/wall-clock; randomness via the
// engine-seeded `rng()`.

namespace gameplay {
struct AttributeSet;
struct AbilitySystem;
class GameplayTagRegistry;
class EventBus;
}

namespace script {

struct ScriptVars;

// What `self` resolves against for one script run: the acting entity's
// components. Any pointer may be null (the proxy degrades gracefully).
struct ScriptContext {
    const gameplay::AttributeSet* attributes { nullptr }; // attribute reads (read-only)
    gameplay::AbilitySystem* abilitySystem { nullptr };   // tags + current values
    ScriptVars* scriptVars { nullptr };                   // self.x read/write
    const gameplay::GameplayTagRegistry* tags { nullptr };
};

struct RunResult {
    bool ok { false };
    str error;
};

class Vm {
public:
    Vm();
    ~Vm();
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;

    // Runs `code` with `self` bound to `context`. Errors are returned, never
    // thrown (§8 recoverable errors).
    RunResult run(const std::string& code, ScriptContext& context);

    // Runs `code` with no `self` (global/module setup).
    RunResult run(const std::string& code);

    // Evaluates `expr` as a boolean predicate with `self` bound (Lua truthiness:
    // nil/false are false). Returns false on error. Feeds the condition
    // evaluator's Lua escape (gameplay supplies this as a callback).
    bool evalPredicate(const std::string& expr, ScriptContext& context);

    // Exposes a Lua `events` table — `events.on(name, fn)` subscribes a Lua
    // function to the bus; on dispatch it receives a table
    // {source, target, value, name, tag}. The bus must outlive this Vm.
    void bindEvents(gameplay::EventBus& bus);

    // Reads a global Lua number (inspection / tests); nullopt if absent or not
    // a number.
    std::optional<f64> getNumber(const std::string& name);

    // Deterministic RNG exposed to Lua as `rng()` -> [0, 1). Seed it from the
    // engine RNG so saves/replays reproduce.
    void seedRng(u64 seed);

private:
    f64 nextRandom();

    struct Impl;
    uptr<Impl> impl;
};

} // namespace script
