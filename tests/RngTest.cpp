#include <doctest/doctest.h>

#include "engine/core/Rng.hpp"

using core::Rng;

TEST_CASE("rng: same seed produces the same sequence (determinism, §8)") {
    Rng a(12345);
    Rng b(12345);
    for (int i = 0; i < 100; ++i) {
        CHECK(a.next() == b.next());
    }
}

TEST_CASE("rng: different seeds diverge") {
    Rng a(1);
    Rng b(2);
    CHECK(a.next() != b.next());
}

TEST_CASE("rng: unit is in [0, 1)") {
    Rng r(42);
    for (int i = 0; i < 1000; ++i) {
        const f64 u = r.unit();
        CHECK(u >= 0.0);
        CHECK(u < 1.0);
    }
}

TEST_CASE("rng: range is inclusive and bounded; degenerate ranges are safe") {
    Rng r(7);
    for (int i = 0; i < 1000; ++i) {
        const i32 v = r.range(3, 8);
        CHECK(v >= 3);
        CHECK(v <= 8);
    }
    CHECK(r.range(5, 5) == 5);
    CHECK(r.range(9, 2) == 9); // hi <= lo → lo
}

TEST_CASE("rng: chance honours its edges and is roughly fair") {
    Rng r(99);
    CHECK_FALSE(r.chance(0.0));
    CHECK(r.chance(1.0));
    int hits = 0;
    for (int i = 0; i < 10000; ++i) {
        if (r.chance(0.5)) {
            ++hits;
        }
    }
    CHECK(hits > 4500);
    CHECK(hits < 5500);
}

TEST_CASE("rng: seed 0 is remapped to a live state (no dead stream)") {
    Rng r(0);
    CHECK(r.next() != 0);
}

TEST_CASE("rng: raw state snapshot/restore reproduces the stream (Phase 8 saves)") {
    Rng r(2024);
    r.next();
    r.next();
    const u64 snap = r.rawState();
    const u64 expected = r.next();
    r.setRawState(snap);
    CHECK(r.next() == expected);
}
