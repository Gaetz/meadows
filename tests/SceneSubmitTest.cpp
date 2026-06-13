#include <doctest/doctest.h>

#include <glm/gtc/quaternion.hpp>

#include "game/SceneSubmit.hpp"

// The render bridge's GPU path (texture resolution, submission) needs a live GL
// context, so it is exercised by running the game (brick e). What is pure and
// unit-testable is the component → sprite mapping, including the 2D yaw.

TEST_CASE("scene submit: maps transform + sprite render to a 2D sprite") {
    world::Transform transform;
    transform.position = { 3.0f, 4.0f, 9.0f }; // z ignored in 2D
    transform.scale = { 2.0f, 3.0f, 1.0f };

    world::SpriteRender sprite;
    sprite.size = { 1.5f, 2.0f };
    sprite.tint = { 0.5f, 0.6f, 0.7f, 1.0f };

    const rhi::TextureHandle texture { 42 };
    const auto out = game::spriteFor(transform, sprite, texture);

    CHECK(out.position.x == 3.0f);
    CHECK(out.position.y == 4.0f);
    CHECK(out.size.x == doctest::Approx(3.0f)); // size.x * scale.x = 1.5 * 2
    CHECK(out.size.y == doctest::Approx(6.0f)); // size.y * scale.y = 2.0 * 3
    CHECK(out.tint.x == 0.5f);
    CHECK(out.tint.w == 1.0f);
    CHECK(out.texture.id == 42);
    CHECK(out.rotation == doctest::Approx(0.0f)); // identity quaternion
}

TEST_CASE("scene submit: 2D rotation is the yaw of the 3D quaternion") {
    world::Transform transform;
    const f32 angle = 1.2f;
    transform.rotation = glm::angleAxis(angle, Vec3 { 0.0f, 0.0f, 1.0f });

    const auto out = game::spriteFor(transform, world::SpriteRender {}, {});
    CHECK(out.rotation == doctest::Approx(angle));
}
