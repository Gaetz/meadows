#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/MeshData.hpp"

namespace render {

// Procedural low-poly props, deterministic per seed (same seed =
// bit-identical mesh — doctested), flat-shaded, vertex-colored in linear
// space. uv.x carries the wind sway weight (0 = rigid, 1 = outer foliage),
// uv.y the normalized height — tree.vert uses both.

// Leaning tapered trunk topped by 3-4 faceted foliage blobs.
MeshData generateTree(u32 seed);

// Squashed craggy boulder, meant to be sunk slightly into the ground.
MeshData generateRock(u32 seed);

// One or two low foliage blobs hugging the ground.
MeshData generateBush(u32 seed);

} // namespace render
