#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp" // brain-script cache key
#include "engine/ecs/World.hpp" // ecs::Entity (flecs name confined to meadows-ecs)

// The Lua scripting VM (§2.8). ONE shared, sandboxed Lua state; scripts
// are stateless modules; `self` is an entity handle (a proxy over the entity's
// components). sol2/Lua are confined to Vm.cpp (pimpl) so they never leak into
// engine headers. Determinism (§8): no os/io/wall-clock; randomness via the
// engine-seeded `rng()`.

namespace gameplay {
struct AttributeSet;
struct AbilitySystem;
class GameplayTagRegistry;
class EventBus;
struct CombatSituation;
}
namespace data {
class FormDatabase;
}

namespace script {

struct ScriptVars;

// What `self` (or `target`) resolves against for one script run: the acting
// entity's components. Any pointer may be null (the proxy degrades gracefully).
// `attributes` is mutable so `self:applyEffect(...)` can route through the effect
// pipeline (§2.9) — direct `self.<attr> = x` is still rejected.
//
// `entity` is the LIVENESS handle for contexts that outlive the call (the
// coroutine scheduler): before each resume the Vm re-resolves the component
// pointers from it — flecs moves component storage on any archetype change
// and frees it on death, so pointers captured at start must never be trusted
// across frames. A null handle (id 0) means "immediate use":
// the pointers are taken as-is.
struct ScriptContext {
    gameplay::AttributeSet* attributes { nullptr };
    gameplay::AbilitySystem* abilitySystem { nullptr };
    ScriptVars* scriptVars { nullptr };
    const gameplay::GameplayTagRegistry* tags { nullptr };
    const data::FormDatabase* forms { nullptr }; // resolves effects for applyEffect
    ecs::Entity entity {};
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

    // --- Latent ability execution: Lua coroutines + a central scheduler (§2.8) ---

    // Starts `code` as a coroutine with `self`/`target` bound. The script may
    // call `wait(t)` to suspend for t game-seconds and `self:applyEffect(guid)`
    // / `self:addTag(...)`. Give the contexts their `entity` handle: on each
    // resume the component pointers are re-resolved from it — a dead `self`
    // abandons the coroutine, a dead `target` degrades to nil reads.
    void startCoroutine(const std::string& code, ScriptContext self,
                        ScriptContext target);

    // Advances suspended coroutines by dt, resuming those whose wait elapsed.
    void tickCoroutines(f32 dt);

    size_t pendingCoroutines() const;

    // Deterministic RNG exposed to Lua as `rng()` -> [0, 1). Seed it from the
    // engine RNG so saves/replays reproduce.
    void seedRng(u64 seed);

    // --- Brain scripts (enemy/boss behaviors — docs/BOSS-SCRIPTING.md) ---

    // `code` must RETURN a decide function `(situation) -> move string`
    // ("approach"/"strike"/"strafe"/"flee" — the C++ executes it; any
    // other/nil return falls back to the C++ brain). Compiled ONCE and
    // cached under `key` (the ActorForm guid — shared by every instance
    // of that actor); called with `self` bound and the situation as a
    // flat table {distance, attackRange, preferredRange, canSee,
    // swinging, cooldown, healthFraction, courage, aware}. Returns ""
    // on compile/runtime error (logged; a runtime error also drops the
    // cached function so the fallback takes over for good). Call this
    // on DECISION TICKS, never per frame.
    str callBrain(const core::Guid& key, const std::string& code,
                  ScriptContext& context,
                  const gameplay::CombatSituation& situation,
                  std::string_view awareState);

private:
    f64 nextRandom();

    struct Impl;
    uptr<Impl> impl;
};

} // namespace script
