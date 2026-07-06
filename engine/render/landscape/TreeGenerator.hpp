#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/MeshData.hpp"

namespace render {

// Procedural low-poly props, deterministic per seed (same seed =
// bit-identical mesh — doctested), flat-shaded, vertex-colored in linear
// space. uv.x carries the wind sway weight (0 = rigid, 1 = outer foliage),
// uv.y the normalized height — tree.vert uses both.

// A tree is ONE opaque mesh (brick 27: full stylized canopy — the BotW
// look; the leaf-card system lost the fill-rate A/B and was removed).
// Tall trunk column, 3-5 short upward branches near the top, one rounded
// foliage lobe per branch tip plus a crown. The canopy's normals are
// SPHERIZED on the mesh: each vertex blends its lobe-center direction with
// the whole-canopy direction (~0.4), so light pools per lobe while staying
// coherent across the tree — the halisavakis/BotW trick applied to solid
// geometry. Early-Z friendly, and the same mesh casts the shadows.
//
// `lobeSubdivisions` is the canopy LOD: 2 = 320-face lobes (near), 1 = 80
// faces (distance, shadow casters, reflections — 4x cheaper on vertices).
// The SAME seed drives both: composition and colors match across LODs,
// only the lobe tessellation changes.
MeshData generateTree(u32 seed, u32 lobeSubdivisions = 2);

// Squashed craggy boulder, meant to be sunk slightly into the ground.
MeshData generateRock(u32 seed);

// One or two low foliage blobs hugging the ground.
MeshData generateBush(u32 seed);

} // namespace render
