#pragma once

// Subsystem map: docs/AUDIT/U5-runtime-scenes-ui.md

#include "engine/render/SpriteRenderer.hpp" // render::Sprite, rhi::TextureHandle
#include "engine/render/landscape/FxInstance.hpp" // render::FxInstance
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
namespace data {
class FormDatabase;
}

namespace game {

class TextureCache;

// A placed local light, extracted for the renderer.
// Spots: `direction` is the placement's forward
// (Transform.rotation × +Z, the scene's yaw convention); spotAngle is the
// FULL cone angle in degrees, 0 = point light.
struct SceneLight {
    Vec3 position { 0.0f };
    Vec3 color { 1.0f };
    f32 intensity { 1.0f };
    f32 radius { 8.0f };
    f32 flicker { 0.0f };
    Vec3 direction { 0.0f, 0.0f, 1.0f };
    f32 spotAngle { 0.0f };
    // The scene overrides `direction` (and gates intensity by
    // sun elevation) for sun-linked lights before filling the UBO.
    bool sunLinked { false };
    // Interior key-light shadow candidate.
    bool castsShadow { false };
    // docs/RENDERING.md: routed through the GI field only (no direct).
    bool rcOnly { false };
    // Window projector half extents (> 0 = window light; `direction`
    // then carries the window's into-room NORMAL, not a beam).
    f32 windowHalfWidth { 0.0f };
    f32 windowHalfHeight { 0.0f };
};

// A placed water volume: surface quad + camera submersion test.
struct WaterVolumeInstance {
    u64 entityId { 0 };
    Vec3 position { 0.0f };    // volume BASE (top face = base + 2*halfY)
    Vec3 halfExtents { 0.0f };
    Vec3 tint { 0.0f };
    f32 chop { 0.5f };
};

// A self-owning render packet — the contractual ECS↔renderer boundary. Extracted
// from the world once per frame; the renderer consumes ONLY this and has no
// access to the World. render::Sprite is pure POD carrying an already-resolved
// rhi::TextureHandle (no live pointers into the ECS or assets), so the packet is
// safe to pass by value across a thread boundary later without touching callers.
struct RenderSnapshot {
    // Sprites already in painter order (lower SpriteRender.layer first; stable
    // query order within a layer — no depth buffer in the 2D phase).
    vector<render::Sprite> sprites;

    // 3D meshes (contract: docs/HORIZONTAL-PASS.md). Guids, not GPU
    // handles: the 3D frontend
    // owns a mesh/material residency cache (the TextureCache pattern) and
    // resolves them at submit — a pending asset draws a placeholder,
    // never blocks (§7). Transform is fully composed world space.
    // Material FIELDS are resolved at extract (resolveMeshMaterials) so the
    // draw needs no FormDatabase access; defaults = the no-material
    // fallback (white albedo, plain tint).
    struct MeshInstance {
        core::Guid model;    // glTF mesh asset
        core::Guid material; // MaterialForm (keys the bind-group cache)
        Mat4 transform { 1.0f };
        Vec4 tint { 1.0f };
        f32 emissive { 0.0f };
        core::Guid albedoTexture {}; // asset guid; 0 = white
    };
    vector<MeshInstance> meshes;

    // The frame's live particles, POD copies from fx::ParticleSim
    // (the extract pre-sorts the ALPHA batch far-to-near; additive is
    // order-free). The renderer draws only these.
    vector<render::FxInstance> fxAlpha;
    vector<render::FxInstance> fxAdditive;

    // The landscape frame's world-derived render data: render()
    // consumes these instead of querying the live World.
    vector<SceneLight> lights;       // the N nearest, for the lights UBO
    vector<SceneLight> shadowLights; // every castsShadow light (key shadow)
    vector<WaterVolumeInstance> waterVolumes;

    // Skinned NPCs: the pose is COPIED (self-owning packet, no
    // pointer into the director's Npc structs). vertices/indices are
    // resolved GPU handles, the sprite/TextureHandle precedent — the skin
    // geometry is residency state built once per NPC by the director; the
    // renderer owns the per-entity draw state (palette SSBO, model UBO,
    // bind groups) keyed by entityId, mark/swept against this list.
    struct SkinnedInstance {
        u64 entityId { 0 };
        Mat4 transform { 1.0f }; // translate(position) × rotation
        Vec4 tint { 1.0f };
        rhi::BufferHandle vertices {};
        rhi::BufferHandle indices {};
        u32 indexCount { 0 };
        vector<Mat4> palette; // skin matrices, this frame's pose
    };
    vector<SkinnedInstance> skinned;
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

// Light selection (docs/RENDERING.md §5 B1). With a `viewProj`, candidates
// are culled sphere-vs-frustum (a light behind a wall of the frustum still
// passes while its radius reaches in) and the budget goes to the highest
// `intensity / (1 + distSq)` scores — a bright far torch IN VIEW beats a
// dim close one behind the camera. Without it (headless callers), every
// light is a candidate and the score alone picks. The RETURNED list is
// always nearest-first (stable, ties keep query order — deterministic):
// consumers rely on that order (flicker phases are per-index, and the GI
// takes the first kMaxLights as "the nearest" for its ~32 m window).
vector<SceneLight> collectLights(const ecs::World& world, const Vec3& focus,
                                 u32 maxLights,
                                 const Mat4* viewProj = nullptr);

// The landscape extract, headless like extractMeshes:
// - extractLights fills `lights` (collectLights above, UBO order) and
//   `shadowLights` (every castsShadow light — key-shadow candidates);
// - extractWaterVolumes fills `waterVolumes`;
// - resolveMeshMaterials folds each mesh's MaterialForm fields into the
//   instance (tint/emissive/albedo guid), so the draw needs no Forms.
void extractLights(const ecs::World& world, const Vec3& focus, u32 maxLights,
                   RenderSnapshot& out, const Mat4* viewProj = nullptr);
void extractWaterVolumes(const ecs::World& world, RenderSnapshot& out);
void resolveMeshMaterials(const data::FormDatabase& forms,
                          RenderSnapshot& out);

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
