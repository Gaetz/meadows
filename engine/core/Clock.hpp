#pragma once

#include <chrono>

#include "engine/core/Defines.hpp"

// The ONE monotonic engine clock: every "how long since X"
// measurement derives from here instead of re-deriving std::chrono per site
// (FrameProbe, Engine dt, TerrainSystem timing, UiSystem). WALL time for
// instrumentation and UI pacing only — game time goes through
// gameplay::GameClock and the frame dt; wall time must never reach the sim
// (§8 determinism). Calendar timestamps (save slots) keep system_clock.

namespace core {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

inline TimePoint clockNow() { return Clock::now(); }

inline f64 secondsSince(TimePoint start) {
    return std::chrono::duration<f64>(Clock::now() - start).count();
}

inline f64 millisecondsSince(TimePoint start) {
    return std::chrono::duration<f64, std::milli>(Clock::now() - start)
        .count();
}

inline f64 secondsBetween(TimePoint start, TimePoint end) {
    return std::chrono::duration<f64>(end - start).count();
}

} // namespace core
