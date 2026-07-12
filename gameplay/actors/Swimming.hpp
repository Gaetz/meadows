#pragma once

#include <optional>

#include "engine/core/Defines.hpp"

// Chantier P0 D2b — swimming, the sim-pure half: WHEN a body swims. The
// dev rule holds: the mode is an enum and ONE flat function decides the
// transitions; the controller only executes (3D wish, drain, drowning)
// and the physics facade only obeys (setSwimming).

namespace gameplay {

enum class MoveMode : u8 { Ground, Swim };

// Hysteresis so the shoreline doesn't flicker:
//   Ground -> Swim  when the HEAD sinks 0.3 m below the surface;
//   Swim   -> Ground when the head clears the surface, or the feet find
//                    ground in water shallower than a body (wading out).
// No surface (dry land, interiors) always grounds.
MoveMode decideMoveMode(MoveMode current, std::optional<f32> surfaceY,
                        f32 feetY, f32 headHeight, bool onGround);

} // namespace gameplay
