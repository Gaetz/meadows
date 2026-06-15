#pragma once

#include "engine/core/Defines.hpp"

namespace core {

// The engine's deterministic, seedable RNG (§8): all gameplay randomness flows
// through a `core::Rng` instance passed by reference — never a global, never
// wall-clock entropy — so saves and replays reproduce. The generator is
// xorshift64* (fast, good enough for gameplay; not crypto). State is a plain u64;
// serialize it (Phase 8) to checkpoint the random stream. `script::Vm` reuses it.
class Rng {
public:
    Rng() = default;
    explicit Rng(u64 seed) { this->seed(seed); }

    // 0 is a fixed point for xorshift, so it is remapped to a live state.
    void seed(u64 s) { state = s != 0 ? s : 1; }

    // Raw 64-bit draw.
    u64 next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1Dull;
    }

    // Uniform in [0, 1) (53 significant bits).
    f64 unit() {
        return static_cast<f64>(next() >> 11) * (1.0 / 9007199254740992.0);
    }

    // Uniform inclusive integer in [lo, hi] (returns lo if hi <= lo).
    i32 range(i32 lo, i32 hi) {
        if (hi <= lo) {
            return lo;
        }
        const u64 span = static_cast<u64>(hi - lo) + 1;
        return lo + static_cast<i32>(next() % span);
    }

    // True with probability `p` (clamped to [0, 1]).
    bool chance(f64 p) {
        if (p <= 0.0) {
            return false;
        }
        if (p >= 1.0) {
            return true;
        }
        return unit() < p;
    }

    // For save/replay (Phase 8): snapshot / restore the stream position.
    u64 rawState() const { return state; }
    void setRawState(u64 s) { state = s != 0 ? s : 1; }

private:
    u64 state { 0x9E3779B97F4A7C15ull };
};

} // namespace core
