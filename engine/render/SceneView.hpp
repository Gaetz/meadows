#pragma once

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"
#include "engine/render/SpriteRenderer.hpp" // render::Sprite
#include "engine/render/landscape/FxInstance.hpp"
#include "engine/rhi/Rhi.hpp"

// The renderer-consumable snapshot types — what a frame IS, seen from the
// renderer (docs/RENDERING.md §7). Engine-side and world-free: the extract
// functions that FILL these from the ECS live above, in game/SceneSubmit
// (the single ECS↔rhi seam); the renderer consumes ONLY these types and
// never reads the scene.

namespace render {

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
    vector<Sprite> sprites;

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
    vector<FxInstance> fxAlpha;
    vector<FxInstance> fxAdditive;

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

} // namespace render
