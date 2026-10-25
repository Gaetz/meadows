#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"
#include "engine/render/Frustum.hpp"
#include "engine/render/SceneView.hpp"
#include "engine/render/landscape/TerrainSystem.hpp"

namespace render {

// The per-frame decisions the world renderer takes from the snapshot
// and the view alone — pure functions: inputs in, one value out, no GPU
// handle touched. The orchestrator uploads what they return.

// The lights-UBO capacity (the extract collects this many selected
// LightSource entities into the snapshot). The full budget is only
// consumable through the clustered path (docs/RENDERING.md §5); with
// clustered off, the per-pixel loop clamps to kFallbackLights.
inline constexpr u32 kMaxLights = 64;
inline constexpr u32 kFallbackLights = 24;

// std140 LightsUbo mirror (binding 5 — mirrors locallights.glsl).
struct LightsUniforms {
    Vec4 count { 0.0f };
    Vec4 positionRadius[kMaxLights] {};
    Vec4 colorIntensity[kMaxLights] {};
    // xyz = spot direction, w = cos(half angle); w = -2 marks a point
    // light, w = -3 a WINDOW projector (xyz = the window's into-room
    // normal — docs/RENDERING.md §3).
    Vec4 directionAngle[kMaxLights] {};
    // Window projector half extents (xy); zw free. APPEND-only UBO.
    Vec4 windowInfo[kMaxLights] {};
};

// H3 (docs/RENDERING.md): 0 below the worldspace's buried threshold,
// 1 above, 4 m fade band — sun-linked lights and the daylight coupling
// fade out per POSITION. Threshold -1e9 = the rule is off (always 1).
inline f32 aboveBuried(f32 y, f32 threshold) {
    return glm::smoothstep(threshold - 2.0f, threshold + 2.0f, y);
}

// One selected key-shadow light: its anchor (the UBO match key —
// the light's original position) and the tile's perspective matrix.
struct KeyShadowPick {
    Vec3 anchor;
    Mat4 viewProj;
};

// Key-shadow atlas selection (docs/RENDERING.md §5 B6): the up-to-
// `maxSlots` best-scored castsShadow lights get a tile each, nearest
// and brightest first. Sun-linked sources film through their window
// along the live beam (a dead beam frees its slot); point lights film
// straight down.
vector<KeyShadowPick> pickKeyShadows(const vector<SceneLight>& shadowLights,
                                     const Vec3& cameraPosition,
                                     const Vec3& shadowSunDirection,
                                     u32 maxSlots);

// The lights-UBO fill: the selected local lights, flicker applied
// CPU-side (sin + per-index phase — cheap and stateless), sun-linked
// sources gated by the sun, each light matched to its key-shadow tile.
struct LightsInputs {
    const vector<SceneLight>& lights; // the snapshot's nearest lights
    u32 budget;                       // kMaxLights or kFallbackLights
    // "Lights via RC only": the DIRECT contribution is cut (count 0 ->
    // localLights() adds nothing) and the lights exist purely in the GI
    // volume. Only meaningful while the RC technique is active.
    bool rcOnly;
    Vec3 shadowSunDirection;
    Vec3 sunColor;
    f32 buriedBelowY;
    f32 timeSeconds;
    const vector<KeyShadowPick>& keyShadowPicks;
};
LightsUniforms fillLightsUbo(const LightsInputs& in);

// Reflection auto-skip: true when some sea can actually show — a
// RESIDENT chunk dipping below sea level inside the frustum, within the
// mirror's useful range. The known miss: sea at the horizon beyond the
// resident ring (~960 m) — the A/B toggle exists for that exact check.
bool seaVisibleInFrustum(const vector<TerrainSystem::ChunkAabb>& chunks,
                         f32 seaLevel, const Vec3& cameraPosition,
                         const Frustum& frustum);

} // namespace render
