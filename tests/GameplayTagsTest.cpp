#include <doctest/doctest.h>

#include "gameplay/ability/GameplayTags.hpp"

using gameplay::GameplayTag;
using gameplay::GameplayTagRegistry;
using gameplay::TagContainer;

TEST_CASE("gameplay tags: registering a leaf registers its ancestors") {
    GameplayTagRegistry registry;
    const GameplayTag burning = registry.registerTag("Status.Burning");

    REQUIRE(burning.isValid());
    CHECK(registry.find("Status.Burning").has_value());
    CHECK(registry.find("Status").has_value()); // ancestor auto-registered
    CHECK_FALSE(registry.find("Status.Frozen").has_value());

    const GameplayTag status = *registry.find("Status");
    CHECK(registry.parentOf(burning) == status);
    CHECK_FALSE(registry.parentOf(status).isValid()); // root
    CHECK(*registry.nameOf(burning) == "Status.Burning");
}

TEST_CASE("gameplay tags: registration is idempotent and ids are stable") {
    GameplayTagRegistry registry;
    const GameplayTag a = registry.registerTag("State.InCombat");
    const GameplayTag b = registry.registerTag("State.InCombat");
    CHECK(a == b);
    // "State" counted once despite two registrations of its child.
    CHECK(registry.count() == 2); // State, State.InCombat
}

TEST_CASE("gameplay tags: isA walks the ancestor chain") {
    GameplayTagRegistry registry;
    const GameplayTag deep = registry.registerTag("A.B.C");
    const GameplayTag b = *registry.find("A.B");
    const GameplayTag a = *registry.find("A");
    const GameplayTag other = registry.registerTag("X.Y");

    CHECK(registry.isA(deep, b));
    CHECK(registry.isA(deep, a));
    CHECK(registry.isA(deep, deep));
    CHECK_FALSE(registry.isA(b, deep));   // parent is not a descendant
    CHECK_FALSE(registry.isA(deep, other));
}

TEST_CASE("tag container: has() is ancestor-aware") {
    GameplayTagRegistry registry;
    const GameplayTag burning = registry.registerTag("Status.Burning");
    const GameplayTag status = *registry.find("Status");
    const GameplayTag frozen = registry.registerTag("Status.Frozen");

    TagContainer container;
    container.add(burning, registry);

    CHECK(container.has(burning));
    CHECK(container.has(status));         // ancestor of an owned tag
    CHECK_FALSE(container.has(frozen));   // sibling, not owned
}

TEST_CASE("tag container: ref-counting keeps a tag until all sources remove it") {
    GameplayTagRegistry registry;
    const GameplayTag burning = registry.registerTag("Status.Burning");
    const GameplayTag bleeding = registry.registerTag("Status.Bleeding");
    const GameplayTag status = *registry.find("Status");

    TagContainer container;
    container.add(burning, registry);   // Status count -> 1 (via Burning)
    container.add(bleeding, registry);  // Status count -> 2 (via Bleeding)

    CHECK(container.has(status));
    container.remove(burning, registry); // Status count -> 1, Burning gone
    CHECK_FALSE(container.has(burning));
    CHECK(container.has(bleeding));
    CHECK(container.has(status));         // still granted by Bleeding

    container.remove(bleeding, registry); // Status count -> 0
    CHECK_FALSE(container.has(status));
    CHECK(container.distinctCount() == 0);
}
