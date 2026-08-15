#include <doctest/doctest.h>

#include "engine/render/landscape/TerrainNoise.hpp"
#include "game/TerrainCollision.hpp"

// Collision tiles sampled from the SAME deterministic terrain function
// the renderer uses — a capsule dropped anywhere rests at terrain height,
// and the tile ring follows the focus with hysteresis.

TEST_CASE("terrain collision keeps a tile ring and matches terrain height") {
    phys::PhysicsWorld world;
    render::TerrainParams params; // defaults, seed 1337 (the demo terrain)
    game::TerrainCollision collision { world, params };

    const Vec3 focus { 32.0f, 0.0f, 368.0f }; // the demo spawn area
    // Synchronous fallback (no JobSystem): the focus tile is guaranteed
    // on the first update, the ring converges one budgeted cook at a
    // time (anti-stutter contract; the game path samples on workers).
    collision.update(focus);
    CHECK(collision.tileCount() >= 1);
    CHECK(collision.tileCount() <= 2);
    for (int i = 0; i < 10; ++i) {
        collision.update(focus);
    }
    CHECK(collision.tileCount() == 9);
    // Same focus again: converged — nothing rebuilt, nothing evicted.
    collision.update(focus);
    CHECK(collision.tileCount() == 9);

    // A capsule dropped from above rests at the terrain function's height.
    const f32 expected = render::terrain::height(params, focus.x, focus.z);
    phys::CharacterBody character {
        world, 0.35f, 1.8f, { focus.x, expected + 10.0f, focus.z }
    };
    constexpr f32 dt = 1.0f / 60.0f;
    for (int i = 0; i < 240; ++i) {
        character.move({ 0.0f, 0.0f, 0.0f }, dt);
        world.tick(dt);
    }
    CHECK(character.onGround());
    // Height-field cells are 1 m bilinear patches of a curved function:
    // allow the discretization gap.
    CHECK(character.position().y ==
          doctest::Approx(expected).epsilon(0.02));

    // Moving three tiles away rebuilds the ring ahead (budgeted) and
    // evicts behind (hysteresis keeps ring 2, count stays bounded).
    constexpr f32 kTileEdge = (game::TerrainCollision::kSamples - 1) *
                              game::TerrainCollision::kSpacing;
    for (int i = 0; i < 12; ++i) {
        collision.update(focus + Vec3 { 3.0f * kTileEdge, 0.0f, 0.0f });
    }
    CHECK(collision.tileCount() <= 12); // 9 new ring + <= 3 kept behind
    CHECK(collision.tileCount() >= 9);
}
