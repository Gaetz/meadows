#pragma once

#include "engine/core/Defines.hpp"
#include "engine/assets/MeshData.hpp"

namespace render {

// Flat-shaded primitive builders for procedural props (trees, rocks).
// Every face gets its own duplicated vertices with the face normal — the
// low-poly faceted look is the point. Deterministic for equal inputs.

// Open tapered tube from `base` to `top` (no caps — tree trunks bury both
// ends). `color` fills the vertex color channel; uv is left {0,0} for the
// caller to overwrite (e.g. sway weights).
void appendTaperedTube(MeshData& mesh, const Vec3& base, const Vec3& top,
                       f32 radiusBase, f32 radiusTop, u32 sides,
                       const Vec3& color);

// Faceted blob: an icosphere with deterministic radial jitter — foliage
// clumps, rocks. `jitter` is the relative radius variation (0 = perfect
// sphere, 0.25 = craggy). `subdivisions`: 1 = 80 faces (craggy rocks),
// 2 = 320 (rounder foliage under leaf cards).
void appendBlob(MeshData& mesh, u32 seed, const Vec3& center, f32 radius,
                f32 jitter, const Vec3& color, u32 subdivisions = 1);

// Axis-aligned box, flat-shaded, each face carrying [0,1] UVs — the
// textured-mesh workhorse (proof cube, kit-module prototypes).
void appendBox(MeshData& mesh, const Vec3& center, const Vec3& halfExtents,
               const Vec3& color);

// Recomputes per-face normals after positions were deformed (e.g. squashed
// rocks). Assumes the flat-shaded layout (three unique vertices per face).
void recomputeFlatNormals(MeshData& mesh);

} // namespace render
