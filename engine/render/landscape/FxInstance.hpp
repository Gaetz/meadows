#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

namespace render {

// One live particle as the renderer draws it: a pure
// two-Vec4 POD, split out of FxRenderer.hpp so the snapshot
// side (game/SceneSubmit) carries batches without dragging the renderer
// (rhi handles, pipelines) into the sim-facing header.
struct FxInstance {
    Vec4 positionSize; // xyz = world center (flames: the base), w = size (m)
    Vec4 color;        // sprites: straight RGBA; flames: core rgb, a = age 0..1
    Vec4 extra;        // sprites: unused; flames: outer rgb, a = seed 0..1
    Vec4 life;         // flames: x = lifetime (s); the rest free
};

} // namespace render
