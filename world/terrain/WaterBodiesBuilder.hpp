#pragma once

#include "data/forms/FormDatabase.hpp"
#include "engine/terrain/WaterBodies.hpp"

// WaterBodyForm/RiverForm records -> the engine's immutable
// render::WaterBodies (sibling of buildHeightPatches/buildTerrainBase:
// the engine never sees Forms). River points are gathered by `parent`
// and sorted by `index`; rivers with fewer than two points are skipped.

namespace world {

sptr<const render::WaterBodies> buildWaterBodies(
    const data::FormDatabase& forms, f32 seaLevel);

} // namespace world
