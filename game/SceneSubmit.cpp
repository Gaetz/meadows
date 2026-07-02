#include "game/SceneSubmit.hpp"

#include <algorithm>
#include <cmath>

#include "engine/ecs/World.hpp"
#include "game/TextureCache.hpp"

namespace game {

render::Sprite spriteFor(const world::Transform& transform,
                         const world::SpriteRender& sprite,
                         rhi::TextureHandle texture) {
    render::Sprite out;
    out.position = { transform.position.x, transform.position.y };
    out.size = { sprite.size.x * transform.scale.x,
                 sprite.size.y * transform.scale.y };
    // Yaw of a quaternion: for a rotation about Z by θ, q = (cos θ/2, 0, 0,
    // sin θ/2), so θ = 2·atan2(z, w). 3D rotations collapse to their yaw in 2D.
    out.rotation = 2.0f * std::atan2(transform.rotation.z, transform.rotation.w);
    out.tint = sprite.tint;
    out.uvRect = sprite.uvRect;
    out.texture = texture;
    return out;
}

RenderSnapshot extractScene(const ecs::World& world, TextureCache& textures) {
    struct Drawable {
        i32 layer;
        render::Sprite sprite;
    };
    vector<Drawable> drawables;

    world.handle()
        .query<const world::Transform, const world::SpriteRender>()
        .each([&](flecs::entity, const world::Transform& transform,
                  const world::SpriteRender& sprite) {
            drawables.push_back(
                { sprite.layer,
                  spriteFor(transform, sprite, textures.resolve(sprite.sprite)) });
        });

    // Painter's order: lower layers first. Stable so same-layer order stays
    // deterministic (query/handle order).
    std::stable_sort(drawables.begin(), drawables.end(),
                     [](const Drawable& a, const Drawable& b) {
                         return a.layer < b.layer;
                     });

    RenderSnapshot snapshot;
    snapshot.sprites.reserve(drawables.size());
    for (const Drawable& drawable : drawables) {
        snapshot.sprites.push_back(drawable.sprite);
    }
    return snapshot;
}

void submitSnapshot(const RenderSnapshot& snapshot,
                    render::SpriteRenderer& renderer) {
    for (const render::Sprite& sprite : snapshot.sprites) {
        renderer.draw(sprite);
    }
}

void prewarmTextures(const ecs::World& world, TextureCache& textures) {
    world.handle().query<const world::SpriteRender>().each(
        [&](flecs::entity, const world::SpriteRender& sprite) {
            textures.resolve(sprite.sprite); // first sighting kicks the decode
        });
}

} // namespace game
