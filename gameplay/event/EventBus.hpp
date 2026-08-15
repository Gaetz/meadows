#pragma once

#include <functional>
#include <string_view>

#include "engine/core/Defines.hpp"
#include "engine/core/Hash.hpp"
#include "engine/ecs/World.hpp"
#include "gameplay/ability/GameplayTags.hpp"

// The gameplay event bus. A deterministic dispatcher that
// quests, scripts and abilities react to. It is Lua-agnostic — handlers are
// plain C++ callbacks; `meadows-script` registers Lua-forwarding handlers.
// Dispatch order = subscription order (§8 determinism).

namespace gameplay {

using EventKind = u32; // fnv1a of the event name, e.g. "OnDeath"
inline EventKind eventKind(std::string_view name) { return core::fnv1a(name); }

// A generic gameplay event. The fixed payload (source/target/tag/value/name)
// covers the common cases (hit, death, activate, trigger, task progress…)
// without a typed struct per event, and maps cleanly to a Lua table.
struct Event {
    EventKind kind { 0 };
    ecs::Entity source {};      // who caused it (attacker, activator)
    ecs::Entity target {};      // who it happened to (victim, activated)
    GameplayTag tag {};         // optional categorizing tag
    f32 value { 0.0f };         // optional scalar (damage, progress…)
    str name {};                // optional string arg (e.g. a data-task arg)
};

using EventHandler = std::function<void(const Event&)>;
using SubscriptionId = u32;

class EventBus {
public:
    SubscriptionId subscribe(EventKind kind, EventHandler handler);
    // Receives EVERY event, whatever its kind — what the quest
    // system needs (task events and quest startEvents are open, data-
    // defined vocabulary; per-name C++ subscriptions can't know them).
    SubscriptionId subscribeAll(EventHandler handler);
    void unsubscribe(SubscriptionId id);

    // Calls every handler for `event.kind` (plus the subscribeAll ones),
    // in subscription order. Re-entrant: a handler ADDED during dispatch
    // first fires on the next one; a handler REMOVED during dispatch is
    // not called after its removal.
    void dispatch(const Event& event) const;

private:
    struct Sub {
        SubscriptionId id;
        EventKind kind;
        EventHandler handler;
    };
    vector<Sub> subs;
    SubscriptionId nextId { 1 };
};

} // namespace gameplay
