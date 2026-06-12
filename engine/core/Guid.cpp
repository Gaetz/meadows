#include "engine/core/Guid.hpp"

#include <random>

namespace core {

namespace {

u8 hexValue(char c) {
    if (c >= '0' && c <= '9') return static_cast<u8>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<u8>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<u8>(c - 'A' + 10);
    return 0xFF;
}

} // namespace

Guid Guid::generate() {
    // Not the gameplay RNG (§8): identity generation must never depend on,
    // nor disturb, the deterministic simulation stream.
    static thread_local std::mt19937_64 rng { std::random_device {}() };
    Guid guid { rng(), rng() };
    // RFC 4122 version 4 / variant 1 bits, for canonical-looking strings.
    guid.hi = (guid.hi & ~0xF000ull) | 0x4000ull;
    guid.lo = (guid.lo & ~(0xC0ull << 56)) | (0x80ull << 56);
    return guid;
}

str Guid::toString() const {
    static constexpr char kHex[] = "0123456789abcdef";
    str out(36, '-');
    u32 outIndex = 0;
    for (u32 i = 0; i < 32; ++i) {
        if (outIndex == 8 || outIndex == 13 || outIndex == 18 ||
            outIndex == 23) {
            ++outIndex;
        }
        const u64 word = i < 16 ? hi : lo;
        const u32 nibbleIndex = 15 - (i % 16);
        out[outIndex++] =
            kHex[(word >> (nibbleIndex * 4)) & 0xF];
    }
    return out;
}

std::optional<Guid> Guid::fromString(std::string_view text) {
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
        text[18] != '-' || text[23] != '-') {
        return std::nullopt;
    }
    Guid guid;
    u32 nibbleCount = 0;
    for (const char c : text) {
        if (c == '-') {
            continue;
        }
        const u8 value = hexValue(c);
        if (value == 0xFF) {
            return std::nullopt;
        }
        u64& word = nibbleCount < 16 ? guid.hi : guid.lo;
        word = (word << 4) | value;
        ++nibbleCount;
    }
    return nibbleCount == 32 ? std::optional<Guid> { guid } : std::nullopt;
}

} // namespace core
