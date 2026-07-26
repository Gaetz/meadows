#pragma once

// Subsystem map: docs/AUDIT/U5-runtime-scenes-ui.md

#include "engine/render/SceneView.hpp" // the snapshot types (render::)
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
namespace data {
class FormDatabase;
}

namespace render {
class TextureCache;
}

namespace game {

// The snapshot types moved to the engine (engine/render/SceneView.hpp) so
// the renderer consumes them without a game/ include; the game-side names
// stay valid for every extract caller (scenes, tests).
using SceneLight = render::SceneLight;
using WaterVolumeInstance = render::WaterVolumeInstance;
using RenderSnapshot = render::RenderSnapshot;

// Pure mapping from scene components to a 2D sprite (no GPU — unit-testable).
// The 2D rotation is the yaw of the 3D-ready quaternion (§2.6); the texture is
// resolved by the caller and passed in.
render::Sprite spriteFor(const world::Transform& transform,
                         const world::SpriteRender& sprite,
                         rhi::TextureHandle texture);

// EXTRACT — the only step that reads the World. Queries every entity with
// Transform + SpriteRender, resolves each texture, and returns a painter-sorted
// snapshot. Read-only: never mutates the world.
RenderSnapshot extractScene(const ecs::World& world,
                            render::TextureCache& textures);

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
void prewarmTextures(const ecs::World& world, render::TextureCache& textures);

// SUBMIT — pure consumer. Draws the snapshot's sprites in order. Touches neither
// the World nor the TextureCache, only the packet and the renderer. Assumes the
// renderer's frame is already begun (the engine owns begin/end).
void submitSnapshot(const RenderSnapshot& snapshot,
                    render::SpriteRenderer& renderer);

} // namespace game
