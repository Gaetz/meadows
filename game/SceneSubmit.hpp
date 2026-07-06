#pragma once

#include "engine/render/SpriteRenderer.hpp" // render::Sprite, rhi::TextureHandle
#include "world/scene/Components.hpp"        // world::Transform, world::SpriteRender

// The render bridge: the single ECS↔rhi seam (§2.6). Keeping it here, above the
// engine and world libs, is what lets `meadows` stay free of any world/ deps —
// the renderer observes the scene from above, gameplay never calls into render.
// Lives in the reusable `meadows-runtime` lib so a future editor can reuse it.
//
// Strict decoupling (§9 Phase 5): the bridge is split into EXTRACT (reads the
// world, produces a self-owning RenderSnapshot) and SUBMIT (consumes only the
// snapshot). Whether the two run on the same thread or across a thread boundary
// is then a scheduling policy, not an architectural choice.

namespace ecs {
class World;
}

namespace game {

class TextureCache;

// A self-owning render packet — the contractual ECS↔renderer boundary. Extracted
// from the world once per frame; the renderer consumes ONLY this and has no
// access to the World. render::Sprite is pure POD carrying an already-resolved
// rhi::TextureHandle (no live pointers into the ECS or assets), so the packet is
// safe to pass by value across a thread boundary later without touching callers.
struct RenderSnapshot {
    // Sprites already in painter order (lower SpriteRender.layer first; stable
    // query order within a layer — no depth buffer in the 2D phase).
    vector<render::Sprite> sprites;

    // 3D meshes (H8 contract). Guids, not GPU handles: the 3D frontend
    // owns a mesh/material residency cache (the TextureCache pattern) and
    // resolves them at submit — a pending asset draws a placeholder,
    // never blocks (§7). Transform is fully composed world space.
    struct MeshInstance {
        core::Guid model;    // glTF mesh asset
        core::Guid material; // MaterialForm
        Mat4 transform { 1.0f };
    };
    vector<MeshInstance> meshes;
};

// Pure mapping from scene components to a 2D sprite (no GPU — unit-testable).
// The 2D rotation is the yaw of the 3D-ready quaternion (§2.6); the texture is
// resolved by the caller and passed in.
render::Sprite spriteFor(const world::Transform& transform,
                         const world::SpriteRender& sprite,
                         rhi::TextureHandle texture);

// EXTRACT — the only step that reads the World. Queries every entity with
// Transform + SpriteRender, resolves each texture, and returns a painter-sorted
// snapshot. Read-only: never mutates the world.
RenderSnapshot extractScene(const ecs::World& world, TextureCache& textures);

// The mesh half of the extract, standalone: pure guids, no texture
// resolution, so it runs (and doctests) without any GPU. The 3D frontend
// calls it directly; extractScene calls it as part of the full extract.
void extractMeshes(const ecs::World& world, RenderSnapshot& out);

// A placed local light, extracted for the renderer (chantier 2 B5).
struct SceneLight {
    Vec3 position { 0.0f };
    Vec3 color { 1.0f };
    f32 intensity { 1.0f };
    f32 radius { 8.0f };
    f32 flicker { 0.0f };
};

// The `maxLights` LightSource entities nearest to `focus`, nearest first
// (stable ordering: ties keep query order — deterministic). Headless.
vector<SceneLight> collectLights(const ecs::World& world, const Vec3& focus,
                                 u32 maxLights);

// Kicks the decode of every sprite asset in the world without drawing anything,
// so a loading gate can wait for them to become resident before the first frame
// is shown (§7) — no startup pop-in. Generalizes to streaming: preload the
// cells around the player before they are on screen.
void prewarmTextures(const ecs::World& world, TextureCache& textures);

// SUBMIT — pure consumer. Draws the snapshot's sprites in order. Touches neither
// the World nor the TextureCache, only the packet and the renderer. Assumes the
// renderer's frame is already begun (the engine owns begin/end).
void submitSnapshot(const RenderSnapshot& snapshot,
                    render::SpriteRenderer& renderer);

} // namespace game
