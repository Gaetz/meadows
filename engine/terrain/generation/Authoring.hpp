#pragma once

#include <span>

#include "engine/core/Defines.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"

// Authoring primitives for hand-shaped terrain (the HighMap-inspired
// toolkit): stamp kernels and ridgelines, interpolate control-point
// elevations, enforce a local elevation while preserving the terrain's
// character. Pure headless functions over GridSpec grids — v1 has NO
// pipeline hook on purpose. The integration point is designed, not
// built: authored records apply to the S1 macro BEFORE erosion (S2 then
// integrates the shapes exactly like procedural ones, and the apron
// contract gives border coherence for free) — that wiring belongs to
// the TerrainGenTool / scenario chantier.

namespace render::terraingen {

enum class StampMode : u8 {
    Add,   // RELATIVE: height += amplitude * falloff (a bump)
    Max,   // ABSOLUTE: height = max(height, amplitude * falloff) —
           //   crossing stamps union into one massif
    Blend, // ABSOLUTE: mix toward `amplitude` by the falloff (plateau)
};

// Radial kernel stamp: `hardness` in [0,1) is the flat core fraction of
// the radius; beyond it the shape smoothsteps to zero at the rim.
// `amplitude` is meters of bump (Add) or the absolute target elevation
// (Max/Blend) — see StampMode.
void stampKernel(const GridSpec& spec, vector<f32>& height, Vec2 center,
                 f32 radius, f32 amplitude, StampMode mode,
                 f32 hardness = 0.3f);

// A ridge along a cubic Bezier: control-point y = crest elevation
// (absolute meters). `crestWidth` meters stay at the crest, then the
// sides fall off over `falloff` meters (smoothstep). Applied as Max —
// crossing ridges merge into massifs.
struct RidgeStroke {
    Vec3 p0 { 0.0f };
    Vec3 p1 { 0.0f };
    Vec3 p2 { 0.0f };
    Vec3 p3 { 0.0f };
    f32 crestWidth { 24.0f };
    f32 falloff { 220.0f };
};
void stampRidge(const GridSpec& spec, vector<f32>& height,
                const RidgeStroke& stroke);

// Scattered elevation anchors -> a smooth base surface: normalized
// radial blend, exact at each anchor center, `background` far from all.
struct ElevationAnchor {
    f32 x { 0.0f };
    f32 z { 0.0f };
    f32 elevation { 0.0f };
    f32 radius { 300.0f };
};
f32 baseElevationAt(std::span<const ElevationAnchor> anchors, f32 x,
                    f32 z, f32 background);

// Enforce `target` elevation at `center` while PRESERVING the local
// shape: the whole disc shifts by a weighted delta instead of being
// flattened — h' = h + w(d) * (target - h(center)). The village-site
// primitive: level the ground, keep the character.
void alterElevation(const GridSpec& spec, vector<f32>& height,
                    Vec2 center, f32 radius, f32 target,
                    f32 blendExponent = 1.5f);

} // namespace render::terraingen
