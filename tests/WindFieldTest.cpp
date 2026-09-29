#include <doctest/doctest.h>

#include "engine/terrain/WindField.hpp"

// Chantier ESPRITS E4.a: the wind field, headless.

using namespace render::terrain;

TEST_CASE("wind: the compass direction, the global wind, a gust that fades and expires") {
    const Vec2 east = windDirectionFromDegrees(0.0f);
    CHECK(east.x == doctest::Approx(1.0f));
    CHECK(east.y == doctest::Approx(0.0f));
    const Vec2 north = windDirectionFromDegrees(90.0f);
    CHECK(north.x == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(north.y == doctest::Approx(-1.0f)); // -z is north

    WindField field;
    field.globalDir = east;
    field.globalSpeed = 3.0f;
    Vec2 w = field.windAt(100.0f, 100.0f);
    CHECK(w.x == doctest::Approx(3.0f));
    CHECK(w.y == doctest::Approx(0.0f));

    // A gust blowing north, 10 m/s at its centre, 8 m of reach.
    field.gusts.push_back({ 100.0f, 100.0f, 0.0f, -1.0f, 10.0f, 8.0f, 2.0f });
    w = field.windAt(100.0f, 100.0f);
    CHECK(w.x == doctest::Approx(3.0f));
    CHECK(w.y == doctest::Approx(-10.0f));
    w = field.windAt(104.0f, 100.0f); // halfway: (1 - 0.25) of it
    CHECK(w.y == doctest::Approx(-7.5f));
    w = field.windAt(120.0f, 100.0f); // beyond: the global wind alone
    CHECK(w.y == doctest::Approx(0.0f));
    field.tick(1.5f);
    CHECK(field.gusts.size() == 1);
    field.tick(1.0f);
    CHECK(field.gusts.empty());
}
