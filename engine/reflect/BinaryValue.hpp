#pragma once

#include "engine/core/BinaryArchive.hpp"
#include "engine/reflect/Reflect.hpp"
#include "engine/reflect/Visit.hpp"

// A reflect::Value on the byte codec: one kind byte (the FieldKind
// ordinal — an on-disk contract, see Reflect.hpp), then the payload.
// Exhaustive on write (a new FieldKind without a case fails to compile);
// the read rejects unknown kind bytes. Quat is written x, y, z, w —
// explicit, independent of glm's storage layout.

namespace reflect {

inline void writeValue(core::ByteWriter& w, const Value& v) {
    w.u8_(static_cast<u8>(valueKind(v)));
    visit(v, overloaded {
        [&](bool b)              { w.u8_(b ? 1 : 0); },
        [&](i32 x)               { w.i32_(x); },
        [&](u32 x)               { w.u32_(x); },
        [&](f32 x)               { w.f32_(x); },
        [&](f64 x)               { w.f64_(x); },
        [&](const str& s)        { w.str_(s); },
        [&](const Vec2& vec)     { w.f32_(vec.x); w.f32_(vec.y); },
        [&](const Vec3& vec)     { w.f32_(vec.x); w.f32_(vec.y); w.f32_(vec.z); },
        [&](const Vec4& vec)     { w.f32_(vec.x); w.f32_(vec.y); w.f32_(vec.z); w.f32_(vec.w); },
        [&](const Quat& q)       { w.f32_(q.x); w.f32_(q.y); w.f32_(q.z); w.f32_(q.w); },
        [&](const core::Guid& g) { w.guid(g); },
    });
}

inline bool readValue(core::ByteReader& r, Value& out) {
    constexpr u8 kMaxKind = static_cast<u8>(FieldKind::F64);
    u8 kindByte = 0;
    if (!r.u8_(kindByte) || kindByte > kMaxKind) return false;
    switch (static_cast<FieldKind>(kindByte)) {
    case FieldKind::Bool: {
        u8 v = 0;
        if (!r.u8_(v)) return false;
        out = (v != 0);
        return true;
    }
    case FieldKind::I32: {
        i32 v = 0;
        if (!r.i32_(v)) return false;
        out = v;
        return true;
    }
    case FieldKind::U32: {
        u32 v = 0;
        if (!r.u32_(v)) return false;
        out = v;
        return true;
    }
    case FieldKind::F32: {
        f32 v = 0;
        if (!r.f32_(v)) return false;
        out = v;
        return true;
    }
    case FieldKind::F64: {
        f64 v = 0;
        if (!r.f64_(v)) return false;
        out = v;
        return true;
    }
    case FieldKind::Str: {
        str v;
        if (!r.str_(v)) return false;
        out = std::move(v);
        return true;
    }
    case FieldKind::Vec2: {
        Vec2 v {};
        if (!r.f32_(v.x) || !r.f32_(v.y)) return false;
        out = v;
        return true;
    }
    case FieldKind::Vec3: {
        Vec3 v {};
        if (!r.f32_(v.x) || !r.f32_(v.y) || !r.f32_(v.z)) return false;
        out = v;
        return true;
    }
    case FieldKind::Vec4: {
        Vec4 v {};
        if (!r.f32_(v.x) || !r.f32_(v.y) || !r.f32_(v.z) || !r.f32_(v.w))
            return false;
        out = v;
        return true;
    }
    case FieldKind::Quat: {
        f32 x, y, z, w;
        if (!r.f32_(x) || !r.f32_(y) || !r.f32_(z) || !r.f32_(w)) return false;
        out = Quat { w, x, y, z }; // glm ctor takes w first
        return true;
    }
    case FieldKind::Guid: {
        core::Guid v;
        if (!r.guid(v)) return false;
        out = v;
        return true;
    }
    }
    return false;
}

} // namespace reflect
