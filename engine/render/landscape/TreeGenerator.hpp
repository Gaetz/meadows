#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/MeshData.hpp"

namespace render {

// Procedural low-poly tree, deterministic per seed (same seed = bit-identical
// mesh — doctested): a leaning tapered trunk topped by 3-4 faceted foliage
// blobs, flat-shaded, vertex-colored in linear space. uv.x carries the wind
// sway weight (0 = rooted trunk, 1 = outer foliage), uv.y the normalized
// height — tree.vert uses both.
MeshData generateTree(u32 seed);

} // namespace render
