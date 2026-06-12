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

} // namespace core
