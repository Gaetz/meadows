#include <doctest/doctest.h>

#include "game/WeaponMeshes.hpp"

// Chantier P0 A2 — the procedural sword: grip at the origin, blade along
// +Y up to bladeLength. Headless geometry sanity (the visual look is the
// dev's pass).

TEST_CASE("the procedural sword spans grip to bladeLength along +Y") {
    const render::MeshData sword = game::makeSwordMesh(0.9f);
    REQUIRE(!sword.vertices.empty());
    REQUIRE(!sword.indices.empty());
    CHECK(sword.indices.size() % 3 == 0);

    Vec3 lo { 1e9f };
    Vec3 hi { -1e9f };
    for (const render::MeshVertex& v : sword.vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    // The tip reaches bladeLength; the pommel hangs below the grip.
    CHECK(hi.y == doctest::Approx(0.9f));
    CHECK(lo.y < -0.1f);
    // Slim profile: the guard is the widest part.
    CHECK(hi.x == doctest::Approx(0.095f));
    CHECK(hi.z < 0.1f);

    // Every index points at a vertex.
    for (const u32 index : sword.indices) {
        CHECK(index < sword.vertices.size());
    }

    // Longer blade -> taller mesh; degenerate lengths clamp sanely.
    Vec3 hi2 { -1e9f };
    for (const render::MeshVertex& v :
         game::makeSwordMesh(1.4f).vertices) {
        hi2 = glm::max(hi2, v.position);
    }
    CHECK(hi2.y == doctest::Approx(1.4f));
    CHECK(!game::makeSwordMesh(0.0f).vertices.empty());

    CHECK(game::swordMeshGuid().isValid());
}

TEST_CASE("the procedural club is a shaft with a fatter metal head") {
    const render::MeshData club = game::makeClubMesh(0.8f);
    REQUIRE(!club.vertices.empty());
    CHECK(club.indices.size() % 3 == 0);

    Vec3 lo { 1e9f };
    Vec3 hi { -1e9f };
    for (const render::MeshVertex& v : club.vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    // Grip at the origin, head at the tip; the head is the widest part
    // and sits in the top third.
    CHECK(hi.y == doctest::Approx(0.8f));
    CHECK(lo.y < -0.1f);
    CHECK(hi.x == doctest::Approx(0.062f));
    f32 widestY = 0.0f;
    f32 widest = 0.0f;
    for (const render::MeshVertex& v : club.vertices) {
        if (std::abs(v.position.x) > widest) {
            widest = std::abs(v.position.x);
            widestY = v.position.y;
        }
    }
    CHECK(widestY > 0.8f * 0.6f);
    for (const u32 index : club.indices) {
        CHECK(index < club.vertices.size());
    }
    CHECK(game::clubMeshGuid().isValid());
    CHECK(game::clubMeshGuid() != game::swordMeshGuid());
}
