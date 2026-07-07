#pragma once

#include <string_view>

#include "engine/core/Defines.hpp"

namespace core {

// FNV-1a. Used for stable type and field ids in the reflection system:
// the hash of a name is what plugins and saves store, so renaming a
// reflected field/type breaks existing data (documented trade-off).
constexpr u32 fnv1a(std::string_view text) {
    u32 hash = 2166136261u;
    for (const char c : text) {
        hash ^= static_cast<u8>(c);
        hash *= 16777619u;
    }
    return hash;
}

// Murmur3 32-bit finalizer: a cheap, deterministic avalanche mixer of an
// existing u32 (distinct from fnv1a, which hashes bytes for stable ids). The
// shared hash family behind procedural scatter, particles, and mesh jitter.
constexpr u32 hashU32(u32 v) {
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}

// Small deterministic per-stream RNG seeded from a u32, advanced by hashU32.
// NOT the engine's seeded/serialized RNG (core::Rng, §8): this is for cosmetic,
// re-derivable procedural streams (scatter, particles) that never persist.
struct HashRng {
    u32 state { 0 };
    f32 next() { // [0, 1)
        state = hashU32(state);
        return static_cast<f32>(state) * (1.0f / 4294967296.0f);
    }
    f32 spread() { return next() * 2.0f - 1.0f; } // [-1, 1)
};

} // namespace core
