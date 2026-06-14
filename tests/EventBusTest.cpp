#include <doctest/doctest.h>

#include "gameplay/event/EventBus.hpp"

using namespace gameplay;

TEST_CASE("event bus: handlers fire for their kind, in subscription order") {
    EventBus bus;
    str order;
    bus.subscribe(eventKind("OnDeath"), [&](const Event&) { order += "A"; });
    bus.subscribe(eventKind("OnHit"), [&](const Event&) { order += "X"; });
    bus.subscribe(eventKind("OnDeath"), [&](const Event&) { order += "B"; });

    bus.dispatch({ eventKind("OnDeath") });
    CHECK(order == "AB"); // OnHit handler skipped; A before B (subscription order)

    bus.dispatch({ eventKind("OnHit") });
    CHECK(order == "ABX");
}

TEST_CASE("event bus: payload is delivered; unsubscribe stops a handler") {
    EventBus bus;
    f32 totalDamage = 0.0f;
    const SubscriptionId id = bus.subscribe(
        eventKind("OnHit"), [&](const Event& e) { totalDamage += e.value; });

    bus.dispatch({ eventKind("OnHit"), {}, {}, {}, 10.0f, "" });
    bus.dispatch({ eventKind("OnHit"), {}, {}, {}, 5.0f, "" });
    CHECK(totalDamage == 15.0f);

    bus.unsubscribe(id);
    bus.dispatch({ eventKind("OnHit"), {}, {}, {}, 100.0f, "" });
    CHECK(totalDamage == 15.0f); // no longer listening
}
