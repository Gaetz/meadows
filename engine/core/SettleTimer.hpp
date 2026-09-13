#pragma once

#include "engine/core/Defines.hpp"

namespace core {

// A knob-drag debouncer: `settled(live, dt)` returns the value as of the
// last quiet period — the live value only once it has stopped changing
// for `quietSeconds`. Consumers whose reaction to a change is expensive
// (a worker rebake, a full ring re-scatter) see ONE reaction per drag
// instead of one per tick. The first call adopts `live` (no boot delay).
// T needs operator== (f32, Vec3, std::array...).
template <class T>
class SettleTimer {
public:
    explicit SettleTimer(f32 quietSeconds = 0.4f)
        : quiet { quietSeconds } {}

    const T& settled(const T& live, f32 dt) {
        if (!started) {
            started = true;
            current = pending = live;
            return current;
        }
        if (!(live == pending)) {
            pending = live;
            stableFor = 0.0f;
            return current;
        }
        stableFor += dt;
        if (stableFor >= quiet) {
            current = pending;
        }
        return current;
    }

private:
    T current {};
    T pending {};
    f32 quiet;
    f32 stableFor { 0.0f };
    bool started { false };
};

} // namespace core
