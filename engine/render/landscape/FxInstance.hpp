#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

namespace render {

// One live particle as the renderer draws it: a pure
// two-Vec4 POD, split out of FxRenderer.hpp so the snapshot
// side (game/SceneSubmit) carries batches without dragging the renderer
// (rhi handles, pipelines) into the sim-facing header.
struct FxInstance {
    Vec4 positionSize; // xyz = world center, w = quad size (m)
    Vec4 color;        // premultiplied nothing — straight RGBA
};

} // namespace render
