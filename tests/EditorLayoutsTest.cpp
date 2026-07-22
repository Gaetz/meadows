#include <doctest/doctest.h>

#include "data/editor/EditorLayouts.hpp"

// The node-position side-store. NOT a plugin (the decided
// exception): tool state keyed by guids, byte-stable TOML for clean git
// diffs, tolerant parsing (a broken layout file must never block the
// editor — worst case, auto-layout takes over).

using core::Guid;

namespace {
const Guid kGraph =
    *Guid::fromString("eeee0001-0000-4000-8000-000000000001");
const Guid kNodeA =
    *Guid::fromString("eeee0002-0000-4000-8000-000000000001");
const Guid kNodeB =
    *Guid::fromString("eeee0003-0000-4000-8000-000000000001");
} // namespace

TEST_CASE("editor layouts: set/get round-trip through TOML") {
    data::EditorLayouts store;
    CHECK_FALSE(store.positionOf(kGraph, kNodeA).has_value());

    store.setPosition(kGraph, kNodeA, Vec2 { 140.0f, 220.0f });
    store.setPosition(kGraph, kNodeB, Vec2 { -60.5f, 0.0f });
    REQUIRE(store.positionOf(kGraph, kNodeA).has_value());
    CHECK(store.positionOf(kGraph, kNodeA)->x == doctest::Approx(140.0f));

    const str toml = store.writeToml();
    data::EditorLayouts reloaded;
    REQUIRE(reloaded.parseToml(toml));
    CHECK(reloaded.graphCount() == 1);
    REQUIRE(reloaded.positionOf(kGraph, kNodeB).has_value());
    CHECK(reloaded.positionOf(kGraph, kNodeB)->x == doctest::Approx(-60.5f));
    CHECK(reloaded.positionOf(kGraph, kNodeB)->y == doctest::Approx(0.0f));

    // Byte-stable: same content serializes identically (git-diff clean).
    CHECK(reloaded.writeToml() == toml);
}

TEST_CASE("editor layouts: junk entries are skipped, never fatal") {
    const char* toml = R"(
[graphs."not-a-guid"]
"eeee0002-0000-4000-8000-000000000001" = [1.0, 2.0]

[graphs."eeee0001-0000-4000-8000-000000000001"]
"also-not-a-guid" = [1.0, 2.0]
"eeee0002-0000-4000-8000-000000000001" = [3.0]
"eeee0003-0000-4000-8000-000000000001" = [7.0, 8.0]
)";
    data::EditorLayouts store;
    REQUIRE(store.parseToml(toml)); // tolerated
    CHECK_FALSE(store.positionOf(kGraph, kNodeA).has_value()); // 1 float
    REQUIRE(store.positionOf(kGraph, kNodeB).has_value());     // survives
    CHECK(store.positionOf(kGraph, kNodeB)->y == doctest::Approx(8.0f));
}

TEST_CASE("editor layouts: a syntax error reports false and stays empty") {
    data::EditorLayouts store;
    store.setPosition(kGraph, kNodeA, Vec2 { 1.0f, 2.0f });
    CHECK_FALSE(store.parseToml("[graphs.\"unterminated"));
    CHECK(store.graphCount() == 0); // parse replaces, never merges
}
