#include "gameplay/event/EventBus.hpp"

#include <algorithm>

namespace gameplay {

SubscriptionId EventBus::subscribe(EventKind kind, EventHandler handler) {
    const SubscriptionId id = nextId++;
    subs.push_back({ id, kind, std::move(handler) });
    return id;
}

SubscriptionId EventBus::subscribeAll(EventHandler handler) {
    // kind 0 = wildcard: no real event has it (kinds are fnv1a of a
    // non-empty name), so it can't collide with a per-kind subscription.
    return subscribe(0, std::move(handler));
}

void EventBus::unsubscribe(SubscriptionId id) {
    std::erase_if(subs, [id](const Sub& sub) { return sub.id == id; });
}

void EventBus::dispatch(const Event& event) const {
    // Snapshot the matching handlers so a handler may (un)subscribe re-entrantly
    // without invalidating this dispatch.
    vector<EventHandler> toCall;
    for (const Sub& sub : subs) {
        if (sub.kind == event.kind || sub.kind == 0) { // 0 = subscribeAll
            toCall.push_back(sub.handler);
        }
    }
    for (const EventHandler& handler : toCall) {
        handler(event);
    }
}

} // namespace gameplay
