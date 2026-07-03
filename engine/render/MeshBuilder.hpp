#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/MeshData.hpp"

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

// Faceted blob: an icosphere (one subdivision, 80 faces) with deterministic
// radial jitter — foliage clumps, rocks. `jitter` is the relative radius
// variation (0 = perfect sphere, 0.25 = craggy).
void appendBlob(MeshData& mesh, u32 seed, const Vec3& center, f32 radius,
                f32 jitter, const Vec3& color);

} // namespace render
