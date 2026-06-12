#include <doctest/doctest.h>

#include "engine/core/Guid.hpp"

using core::Guid;

TEST_CASE("guid: default is invalid, generated is valid and unique") {
    CHECK(!Guid {}.isValid());

    const Guid a = Guid::generate();
    const Guid b = Guid::generate();
    CHECK(a.isValid());
    CHECK(b.isValid());
    CHECK(a != b);
}

TEST_CASE("guid: string roundtrip is identity") {
    const Guid guid = Guid::generate();
    const str text = guid.toString();

    CHECK(text.size() == 36);
    CHECK(text[8] == '-');
    CHECK(text[13] == '-');
    CHECK(text[18] == '-');
    CHECK(text[23] == '-');
    CHECK(text[14] == '4'); // RFC 4122 version nibble

    const auto parsed = Guid::fromString(text);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == guid);
}

TEST_CASE("guid: known value formats as expected") {
    const Guid guid { 0x1b4e28ba2fa141d2ull, 0x883f0016d3cca427ull };
    CHECK(guid.toString() == "1b4e28ba-2fa1-41d2-883f-0016d3cca427");
    const auto parsed =
        Guid::fromString("1b4e28ba-2fa1-41d2-883f-0016d3cca427");
    REQUIRE(parsed.has_value());
    CHECK(*parsed == guid);
}

TEST_CASE("guid: malformed strings are rejected") {
    CHECK(!Guid::fromString("").has_value());
    CHECK(!Guid::fromString("not-a-guid").has_value());
    // Wrong separator positions.
    CHECK(!Guid::fromString("1b4e28ba2-fa1-41d2-883f-0016d3cca427").has_value());
    // Non-hex character.
    CHECK(!Guid::fromString("1b4e28ba-2fa1-41d2-883f-0016d3cca42g").has_value());
    // Uppercase accepted (lenient parse).
    CHECK(Guid::fromString("1B4E28BA-2FA1-41D2-883F-0016D3CCA427").has_value());
}
