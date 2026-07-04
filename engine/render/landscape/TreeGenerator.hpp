#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/MeshData.hpp"

namespace render {

// Procedural low-poly props, deterministic per seed (same seed =
// bit-identical mesh — doctested), flat-shaded, vertex-colored in linear
// space. uv.x carries the wind sway weight (0 = rigid, 1 = outer foliage),
// uv.y the normalized height — tree.vert uses both.

// A tree splits into two draws (stylized-leaves recipe, halisavakis.com):
// `body` = trunk + darkened canopy blobs, drawn with the standard prop
// shader and reused as the shadow caster (solid blobs cast softer, cheaper
// shadows than alpha-tested cards). `leaves` = leaf cards scattered on the
// blob surfaces, alpha-cutout textured, with SPHERICAL normals (direction
// from the blob center — all four card corners share it) so the canopy
// lights as one soft volume; their uv addresses the leaf-bouquet atlas.
struct TreeMeshes {
    MeshData body;
    MeshData leaves;
};

// Tall trunk column, 3-5 short upward branches near the top, a faceted
// foliage blob at each branch tip (plus a crown) carrying the leaf cards.
TreeMeshes generateTree(u32 seed);

// Squashed craggy boulder, meant to be sunk slightly into the ground.
MeshData generateRock(u32 seed);

// One or two low foliage blobs hugging the ground.
MeshData generateBush(u32 seed);

// Procedural leaf-bouquet atlas (2x2 cells of ellipse leaf clusters),
// RGBA8: rgb = luminance variation multiplied by the card's vertex color,
// a = cutout mask. Deterministic; size = kLeafTextureSize squared.
inline constexpr u32 kLeafTextureSize = 256;
vector<u8> buildLeafTexturePixels();

} // namespace render
