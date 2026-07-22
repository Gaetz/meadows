#pragma once

#include <optional>

#include "engine/core/Defines.hpp"

// Swimming, the sim-pure half: WHEN a body swims.
// The mode is an enum and ONE flat function decides the
// transitions; the controller only executes (3D wish, drain, drowning)
// and the physics facade only obeys (setSwimming).

namespace gameplay {

enum class MoveMode : u8 { Ground, Swim };

// Hysteresis so the shoreline doesn't flicker:
//   Ground -> Swim  when the HEAD sinks submergeDepth m below the surface;
//   Swim   -> Ground when the head clears the surface, or the feet find
//                    ground in water shallower than wadeOutRatio of a body
//                    (wading out).
// No surface (dry land, interiors) always grounds. The thresholds are
// StatsTuningForm fields; the defaults cover callers/tests that don't
// pass them.
MoveMode decideMoveMode(MoveMode current, std::optional<f32> surfaceY,
                        f32 feetY, f32 headHeight, bool onGround,
                        f32 submergeDepth = 0.3f, f32 wadeOutRatio = 0.65f);

} // namespace gameplay
