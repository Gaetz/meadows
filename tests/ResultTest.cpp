#include <doctest/doctest.h>

#include "data/plugins/PluginLoader.hpp"
#include "engine/core/Result.hpp"

// core::Result (audit U1-03): the recoverable-error primitive — optional's
// read API plus the REASON. Introduced with the §5 plugin loader as its
// first real call path; these lock both the type and the wiring.

TEST_CASE("result: carries a value with optional's read API") {
    core::Result<int> ok = 42;
    REQUIRE(ok.has_value());
    CHECK(static_cast<bool>(ok));
    CHECK(*ok == 42);
    CHECK(ok.value() == 42);
}

TEST_CASE("result: carries the failure reason") {
    const auto make = [](bool succeed) -> core::Result<str> {
        if (succeed) {
            return str { "payload" };
        }
        return core::Error { "the reason" };
    };
    const auto bad = make(false);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error() == "the reason");
    const auto good = make(true);
    REQUIRE(good.has_value());
    CHECK(*good == "payload");
    CHECK(good->size() == 7); // operator->
}

TEST_CASE("result: the plugin loader reports WHY a plugin failed") {
    data::FormTypeRegistry types;

    // Malformed TOML: the parse reason (with the source label) comes back.
    const auto broken =
        data::parsePluginToml("this is [not toml", types, "broken.toml");
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error().find("broken.toml") != str::npos);
    CHECK(broken.error().find("TOML parse error") != str::npos);

    // Valid TOML without the [plugin] header: a different, precise reason.
    const auto headless =
        data::parsePluginToml("[something]\nx = 1\n", types, "noheader.toml");
    REQUIRE_FALSE(headless.has_value());
    CHECK(headless.error().find("missing [plugin] header") != str::npos);
}
