#pragma once

#include "engine/render/SpriteRenderer.hpp" // render::Sprite, rhi::TextureHandle
#include "world/scene/Components.hpp"        // world::Transform, world::SpriteRender

// The render bridge: the single ECS↔rhi seam (§2.6). Keeping it here, above the
// engine and world libs, is what lets `meadows` stay free of any world/ deps —
// the renderer observes the scene from above, gameplay never calls into render.
// Lives in the reusable `meadows-runtime` lib so a future editor can reuse it.

namespace ecs {
class World;
}

namespace game {

class TextureCache;

// Pure mapping from scene components to a 2D sprite (no GPU — unit-testable).
// The 2D rotation is the yaw of the 3D-ready quaternion (§2.6); the texture is
// resolved by the caller and passed in.
render::Sprite spriteFor(const world::Transform& transform,
                         const world::SpriteRender& sprite,
                         rhi::TextureHandle texture);

// Queries every entity with Transform + SpriteRender, resolves its texture, and
// submits in painter order (sorted by SpriteRender.layer — there is no depth
// buffer in the 2D phase). Read-only: called from Game::draw, never mutates the
// world. Assumes the renderer's frame is already begun (the engine owns
// begin/end).
void submitScene(const ecs::World& world, TextureCache& textures,
                 render::SpriteRenderer& renderer);

} // namespace game
