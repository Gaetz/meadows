#pragma once

#include <cstddef>
#include <string_view>
#include <type_traits>

#include "engine/core/Defines.hpp"

namespace core {

// FNV-1a 64-bit over raw bytes: the stable key family of the terrain
// bake caches (parameter hashes, map keys) and of the Vulkan pipeline
// cache. ONE basis for every site — the 20-digit constant had been
// mistyped once and copied nine times, which is why the loop lives here.
constexpr u64 kFnv64Basis = 14695981039346656037ull;
constexpr u64 kFnv64Prime = 1099511628211ull;

struct Fnv64 {
    u64 value { kFnv64Basis };

    void bytes(const void* data, size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) {
            value ^= p[i];
            value *= kFnv64Prime;
        }
    }
    // Scalars by their bytes, a bool as one byte, a string by content:
    // the overload set a parameter walker (visitParams) feeds.
    template <class T>
        requires((std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) ||
                 std::is_enum_v<T>)
    void mix(T v) {
        bytes(&v, sizeof(v));
    }
    void mix(bool v) {
        const u8 b = v ? 1 : 0;
        bytes(&b, 1);
    }
    void mix(std::string_view s) { bytes(s.data(), s.size()); }
};

// FNV-1a 64-bit of a text: the 64-bit sibling of fnv1a for cache keys
// and file names that must not collide across a whole world.
constexpr u64 fnv1a64(std::string_view text) {
    u64 hash = kFnv64Basis;
    for (const char c : text) {
        hash ^= static_cast<u8>(c);
        hash *= kFnv64Prime;
    }
    return hash;
}

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
