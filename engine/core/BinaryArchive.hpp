#pragma once

#include <bit>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"

// The byte-level codec every house format is built on: little-endian
// scalars (both target platforms are), length-prefixed strings, a guid as
// its two words. The reader is bounds-checked and returns false on
// truncation instead of reading past the buffer — a corrupt file is data,
// never UB. Formats layer their magic/version/record layout on top.

namespace core {

class ByteWriter {
public:
    explicit ByteWriter(vector<u8>& out) : out { out } {}

    void u8_(u8 value) { out.push_back(value); }
    void u16_(u16 value) {
        for (u32 i = 0; i < 2; ++i) {
            out.push_back(static_cast<u8>(value >> (i * 8)));
        }
    }
    void u32_(u32 value) {
        for (u32 i = 0; i < 4; ++i) {
            out.push_back(static_cast<u8>(value >> (i * 8)));
        }
    }
    void u64_(u64 value) {
        for (u32 i = 0; i < 8; ++i) {
            out.push_back(static_cast<u8>(value >> (i * 8)));
        }
    }
    void i32_(i32 value) { u32_(std::bit_cast<u32>(value)); }
    void f32_(f32 value) { u32_(std::bit_cast<u32>(value)); }
    void f64_(f64 value) { u64_(std::bit_cast<u64>(value)); }
    // u32 length prefix, then the bytes.
    void str_(std::string_view value) {
        u32_(static_cast<u32>(value.size()));
        out.insert(out.end(), value.begin(), value.end());
    }
    void guid(const Guid& value) {
        u64_(value.hi);
        u64_(value.lo);
    }
    // Raw bytes, no prefix (magic numbers, fixed-size blobs).
    void bytes(const void* data, size_t size) {
        const auto* p = static_cast<const u8*>(data);
        out.insert(out.end(), p, p + size);
    }

private:
    vector<u8>& out;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const u8> bytes) : bytes { bytes } {}

    bool u8_(u8& value) {
        if (remaining() < 1) return false;
        value = bytes[pos++];
        return true;
    }
    bool u16_(u16& value) {
        if (remaining() < 2) return false;
        value = 0;
        for (u32 i = 0; i < 2; ++i) {
            value = static_cast<u16>(value |
                                     (static_cast<u16>(bytes[pos++]) << (i * 8)));
        }
        return true;
    }
    bool u32_(u32& value) {
        if (remaining() < 4) return false;
        value = 0;
        for (u32 i = 0; i < 4; ++i) {
            value |= static_cast<u32>(bytes[pos++]) << (i * 8);
        }
        return true;
    }
    bool u64_(u64& value) {
        if (remaining() < 8) return false;
        value = 0;
        for (u32 i = 0; i < 8; ++i) {
            value |= static_cast<u64>(bytes[pos++]) << (i * 8);
        }
        return true;
    }
    bool i32_(i32& value) {
        u32 bits = 0;
        if (!u32_(bits)) return false;
        value = std::bit_cast<i32>(bits);
        return true;
    }
    bool f32_(f32& value) {
        u32 bits = 0;
        if (!u32_(bits)) return false;
        value = std::bit_cast<f32>(bits);
        return true;
    }
    bool f64_(f64& value) {
        u64 bits = 0;
        if (!u64_(bits)) return false;
        value = std::bit_cast<f64>(bits);
        return true;
    }
    bool str_(str& value) {
        u32 size = 0;
        if (!u32_(size) || remaining() < size) return false;
        value.assign(reinterpret_cast<const char*>(bytes.data() + pos), size);
        pos += size;
        return true;
    }
    bool guid(Guid& value) { return u64_(value.hi) && u64_(value.lo); }
    bool bytes_(void* data, size_t size) {
        if (remaining() < size) return false;
        std::memcpy(data, bytes.data() + pos, size);
        pos += size;
        return true;
    }

    size_t remaining() const { return bytes.size() - pos; }
    size_t offset() const { return pos; }

private:
    std::span<const u8> bytes;
    size_t pos { 0 };
};

} // namespace core
