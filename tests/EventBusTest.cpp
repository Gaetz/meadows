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

// The quest system listens to EVERYTHING (open, data-defined
// event vocabulary): subscribeAll receives any kind, in subscription
// order with the per-kind handlers.
TEST_CASE("event bus: subscribeAll receives every kind") {
    gameplay::EventBus bus;
    vector<int> calls;
    bus.subscribe(gameplay::eventKind("OnDeath"),
                  [&](const gameplay::Event&) { calls.push_back(1); });
    bus.subscribeAll([&](const gameplay::Event&) { calls.push_back(2); });

    bus.dispatch({ gameplay::eventKind("OnDeath") });
    bus.dispatch({ gameplay::eventKind("OnAnythingAtAll") });
    // OnDeath: per-kind then wildcard (subscription order); the unknown
    // kind reaches only the wildcard.
    REQUIRE(calls.size() == 3);
    CHECK(calls[0] == 1);
    CHECK(calls[1] == 2);
    CHECK(calls[2] == 2);

    // unsubscribe works on wildcard subscriptions too.
    gameplay::EventBus bus2;
    const auto id = bus2.subscribeAll([&](const gameplay::Event&) {
        calls.push_back(3);
    });
    bus2.unsubscribe(id);
    bus2.dispatch({ gameplay::eventKind("OnDeath") });
    CHECK(calls.size() == 3);
}
