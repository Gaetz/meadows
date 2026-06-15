#pragma once

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"

// A minimal in-game clock (docs/STATS.md §6): real time scaled by `timescale`
// (default ×10 — one real second is ten in-game seconds). Backs the time-based
// stats (regen, survival, rest, injury recovery). Reflected for save (Phase 5);
// `gameSeconds` is f64 so it stays precise across long playthroughs.

namespace gameplay {

struct GameClock {
    f64 gameSeconds { 0.0 };
    f32 timescale { 10.0f };

    // Advances by a real-time delta; returns the in-game seconds elapsed this
    // call (the per-tick delta that drives decay, e.g. survival in S4).
    f64 advance(f32 realDt) {
        const f64 gameDt = static_cast<f64>(realDt) * static_cast<f64>(timescale);
        gameSeconds += gameDt;
        return gameDt;
    }

    f64 gameHours() const { return gameSeconds / 3600.0; }
    f64 gameDays() const { return gameSeconds / 86400.0; }

    REFLECT_BEGIN(GameClock, void)
        REFLECT_FIELD(gameSeconds)
        REFLECT_FIELD(timescale)
    REFLECT_END()
};

} // namespace gameplay
