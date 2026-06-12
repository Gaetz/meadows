#include "data/plugins/BinaryFormat.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

#include "engine/core/Log.hpp"

namespace data {

namespace {

constexpr u8 kMagic[4] = { 'M', 'D', 'W', 'P' };
constexpr u8 kMaxKind = static_cast<u8>(reflect::FieldKind::Guid);

class Writer {
public:
    explicit Writer(vector<u8>& out) : out { out } {}

    void u8_(u8 value) { out.push_back(value); }
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
    void f32_(f32 value) { u32_(std::bit_cast<u32>(value)); }
    void str_(const str& value) {
        u32_(static_cast<u32>(value.size()));
        out.insert(out.end(), value.begin(), value.end());
    }
    void guid(const core::Guid& value) {
        u64_(value.hi);
        u64_(value.lo);
    }

    void value(const reflect::Value& v) {
        using reflect::FieldKind;
        u8_(static_cast<u8>(reflect::valueKind(v)));
        switch (reflect::valueKind(v)) {
        case FieldKind::Bool: u8_(std::get<bool>(v) ? 1 : 0); break;
        case FieldKind::I32:  u32_(std::bit_cast<u32>(std::get<i32>(v))); break;
        case FieldKind::U32:  u32_(std::get<u32>(v)); break;
        case FieldKind::F32:  f32_(std::get<f32>(v)); break;
        case FieldKind::Str:  str_(std::get<str>(v)); break;
        case FieldKind::Vec2: {
            const Vec2& vec = std::get<Vec2>(v);
            f32_(vec.x); f32_(vec.y);
            break;
        }
        case FieldKind::Vec3: {
            const Vec3& vec = std::get<Vec3>(v);
            f32_(vec.x); f32_(vec.y); f32_(vec.z);
            break;
        }
        case FieldKind::Vec4: {
            const Vec4& vec = std::get<Vec4>(v);
            f32_(vec.x); f32_(vec.y); f32_(vec.z); f32_(vec.w);
            break;
        }
        case FieldKind::Quat: {
            // Component order x, y, z, w — explicit, independent of glm's
            // storage layout.
            const Quat& q = std::get<Quat>(v);
            f32_(q.x); f32_(q.y); f32_(q.z); f32_(q.w);
            break;
        }
        case FieldKind::Guid: guid(std::get<core::Guid>(v)); break;
        }
    }

private:
    vector<u8>& out;
};

class Reader {
public:
    Reader(std::span<const u8> bytes) : bytes { bytes } {}

    bool u8_(u8& value) {
        if (remaining() < 1) return false;
        value = bytes[pos++];
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
    bool f32_(f32& value) {
        u32 bits = 0;
        if (!u32_(bits)) return false;
        value = std::bit_cast<f32>(bits);
        return true;
    }
    bool str_(str& value) {
        u32 size = 0;
        if (!u32_(size) || remaining() < size) return false;
        value.assign(reinterpret_cast<const char*>(bytes.data() + pos), size);
        pos += size;
        return true;
    }
    bool guid(core::Guid& value) { return u64_(value.hi) && u64_(value.lo); }

    bool value(reflect::Value& out) {
        using reflect::FieldKind;
        u8 kindByte = 0;
        if (!u8_(kindByte) || kindByte > kMaxKind) return false;
        switch (static_cast<FieldKind>(kindByte)) {
        case FieldKind::Bool: {
            u8 v = 0;
            if (!u8_(v)) return false;
            out = (v != 0);
            return true;
        }
        case FieldKind::I32: {
            u32 v = 0;
            if (!u32_(v)) return false;
            out = std::bit_cast<i32>(v);
            return true;
        }
        case FieldKind::U32: {
            u32 v = 0;
            if (!u32_(v)) return false;
            out = v;
            return true;
        }
        case FieldKind::F32: {
            f32 v = 0;
            if (!f32_(v)) return false;
            out = v;
            return true;
        }
        case FieldKind::Str: {
            str v;
            if (!str_(v)) return false;
            out = std::move(v);
            return true;
        }
        case FieldKind::Vec2: {
            Vec2 v {};
            if (!f32_(v.x) || !f32_(v.y)) return false;
            out = v;
            return true;
        }
        case FieldKind::Vec3: {
            Vec3 v {};
            if (!f32_(v.x) || !f32_(v.y) || !f32_(v.z)) return false;
            out = v;
            return true;
        }
        case FieldKind::Vec4: {
            Vec4 v {};
            if (!f32_(v.x) || !f32_(v.y) || !f32_(v.z) || !f32_(v.w))
                return false;
            out = v;
            return true;
        }
        case FieldKind::Quat: {
            f32 x, y, z, w;
            if (!f32_(x) || !f32_(y) || !f32_(z) || !f32_(w)) return false;
            out = Quat { w, x, y, z }; // glm ctor takes w first
            return true;
        }
        case FieldKind::Guid: {
            core::Guid v;
            if (!guid(v)) return false;
            out = v;
            return true;
        }
        }
        return false;
    }

private:
    size_t remaining() const { return bytes.size() - pos; }

    std::span<const u8> bytes;
    size_t pos { 0 };
};

} // namespace

vector<u8> writePluginBinary(const Plugin& plugin) {
    vector<u8> out;
    Writer w { out };

    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
    w.u32_(kPluginBinaryVersion);

    w.guid(plugin.id);
    w.str_(plugin.name);
    w.u32_(static_cast<u32>(plugin.dependencies.size()));
    for (const core::Guid& dep : plugin.dependencies) {
        w.guid(dep);
    }

    w.u32_(static_cast<u32>(plugin.records.size()));
    for (const Record& record : plugin.records) {
        w.guid(record.formId);
        w.u32_(record.typeId);
        w.u8_(record.creates ? 1 : 0);

        // Sorted by field id: byte-identical output for identical input.
        vector<std::pair<u32, const reflect::Value*>> sorted;
        sorted.reserve(record.fields.size());
        for (const auto& [id, value] : record.fields) {
            sorted.emplace_back(id, &value);
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        w.u32_(static_cast<u32>(sorted.size()));
        for (const auto& [id, value] : sorted) {
            w.u32_(id);
            w.value(*value);
        }
    }
    return out;
}

std::optional<Plugin> readPluginBinary(std::span<const u8> bytes,
                                       std::string_view sourceName) {
    Reader r { bytes };

    u8 magic[4] = {};
    if (!r.u8_(magic[0]) || !r.u8_(magic[1]) || !r.u8_(magic[2]) ||
        !r.u8_(magic[3]) || std::memcmp(magic, kMagic, 4) != 0) {
        LOG_ERROR("{}: not a cooked plugin (bad magic)", sourceName);
        return std::nullopt;
    }
    u32 version = 0;
    if (!r.u32_(version) || version != kPluginBinaryVersion) {
        LOG_ERROR("{}: unsupported cooked version {} (expected {})",
                  sourceName, version, kPluginBinaryVersion);
        return std::nullopt;
    }

    const auto truncated = [&sourceName]() -> std::optional<Plugin> {
        LOG_ERROR("{}: truncated or corrupt cooked plugin", sourceName);
        return std::nullopt;
    };

    Plugin plugin;
    if (!r.guid(plugin.id) || !r.str_(plugin.name)) {
        return truncated();
    }
    u32 dependencyCount = 0;
    if (!r.u32_(dependencyCount)) {
        return truncated();
    }
    for (u32 i = 0; i < dependencyCount; ++i) {
        core::Guid dep;
        if (!r.guid(dep)) {
            return truncated();
        }
        plugin.dependencies.push_back(dep);
    }

    u32 recordCount = 0;
    if (!r.u32_(recordCount)) {
        return truncated();
    }
    for (u32 i = 0; i < recordCount; ++i) {
        Record record;
        u8 creates = 0;
        if (!r.guid(record.formId) || !r.u32_(record.typeId) ||
            !r.u8_(creates)) {
            return truncated();
        }
        record.creates = (creates != 0);

        u32 fieldCount = 0;
        if (!r.u32_(fieldCount)) {
            return truncated();
        }
        for (u32 f = 0; f < fieldCount; ++f) {
            u32 fieldId = 0;
            reflect::Value value;
            if (!r.u32_(fieldId) || !r.value(value)) {
                return truncated();
            }
            record.fields.insert_or_assign(fieldId, std::move(value));
        }
        plugin.records.push_back(std::move(record));
    }

    return plugin;
}

} // namespace data
