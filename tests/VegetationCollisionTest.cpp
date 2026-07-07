#include <doctest/doctest.h>

#include "engine/render/landscape/TerrainSystem.hpp"
#include "game/VegetationCollision.hpp"

// Chantier 6 follow-up: trunk/rock colliders from the deterministic
// scatter. The ring mechanics mirror TerrainCollision (one chunk cooked
// per update, hysteresis eviction); body counts depend on the seed's
// forest mask, so the test asserts convergence and determinism, not
// exact numbers.

TEST_CASE("vegetation collision converges its chunk ring and evicts") {
    phys::PhysicsWorld world;
    render::TerrainParams params; // demo seed 1337
    game::VegetationCollision collision { world, params };

    const Vec3 focus { 32.0f, 0.0f, 368.0f }; // the demo spawn area
    for (int i = 0; i < 12; ++i) {
        collision.update(focus);
    }
    CHECK(collision.chunkCount() == 9);
    const u32 bodiesAtSpawn = collision.bodyCount();

    // Converged: another update changes nothing.
    collision.update(focus);
    CHECK(collision.chunkCount() == 9);
    CHECK(collision.bodyCount() == bodiesAtSpawn);

    // Moving three chunks away rebuilds ahead and evicts behind
    // (hysteresis keeps ring 2, so the count stays bounded).
    const Vec3 away =
        focus + Vec3 { 3.0f * render::TerrainSystem::kChunkSize, 0.0f, 0.0f };
    for (int i = 0; i < 12; ++i) {
        collision.update(away);
    }
    CHECK(collision.chunkCount() >= 9);
    CHECK(collision.chunkCount() <= 12);

    // Determinism: a fresh instance over the same spot cooks the same
    // number of bodies (the scatter is a pure function of the seed).
    game::VegetationCollision again { world, params };
    for (int i = 0; i < 12; ++i) {
        again.update(focus);
    }
    CHECK(again.bodyCount() == bodiesAtSpawn);
}
