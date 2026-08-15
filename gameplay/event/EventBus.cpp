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
    // Snapshot the matching subscription IDS — not the handlers: copying
    // a std::function per handler per event allocates on the hot path
    // (OnHitTaken, footsteps...). Re-entrancy stays safe: a handler
    // subscribing during this dispatch is not seen (its id is not in the
    // snapshot), and one UNSUBSCRIBED during it is skipped (its id no
    // longer resolves).
    vector<SubscriptionId> toCall;
    toCall.reserve(subs.size());
    for (const Sub& sub : subs) {
        if (sub.kind == event.kind || sub.kind == 0) { // 0 = subscribeAll
            toCall.push_back(sub.id);
        }
    }
    for (const SubscriptionId id : toCall) {
        const auto it = std::find_if(
            subs.begin(), subs.end(),
            [id](const Sub& sub) { return sub.id == id; });
        if (it != subs.end()) {
            it->handler(event);
        }
    }
}

} // namespace gameplay
