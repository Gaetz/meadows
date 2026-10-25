#pragma once

#include <string_view>

#include "engine/core/Assert.hpp"
#include "engine/core/Hash.hpp"
#include "engine/reflect/Reflect.hpp"
#include "engine/reflect/Visit.hpp"

// The whole-object operations every consumer of the reflection used to
// hand-write: clone, hash, and the asserted field-id lookup. One body
// each, so a new FieldKind or a new flag is handled here and nowhere else.

namespace reflect {

// Clones every reflected field (parents first) from `src` into `dst`,
// both instances of `type`. Transient fields are copied too: this is a
// clone, not a serialization.
inline void copyFields(const TypeInfo& type, const void* src, void* dst) {
    forEachField(type, [&](const FieldInfo& field) {
        field.set(dst, field.get(src));
    });
}

// Mixes every non-Transient field of `object` (parents first) into `h`:
// the field id, then the value's bytes (a bool as one byte, a string by
// content, a guid by its two words). The same walk yields the same key
// on every platform — the terrain cache keys are built on it.
inline void hashReflected(const TypeInfo& type, const void* object,
                          core::Fnv64& h) {
    forEachField(type, [&](const FieldInfo& field) {
        if ((field.flags & Transient) != 0) {
            return;
        }
        h.mix(field.id);
        visit(field.get(object), overloaded {
            [&](bool v) { h.mix(v); },
            [&](i32 v) { h.mix(v); },
            [&](u32 v) { h.mix(v); },
            [&](f32 v) { h.mix(v); },
            [&](f64 v) { h.mix(v); },
            [&](const Vec2& v) { h.bytes(&v, sizeof(v)); },
            [&](const Vec3& v) { h.bytes(&v, sizeof(v)); },
            [&](const Vec4& v) { h.bytes(&v, sizeof(v)); },
            [&](const Quat& v) { h.bytes(&v, sizeof(v)); },
            [&](const str& v) { h.mix(std::string_view { v }); },
            [&](const core::Guid& v) {
                h.mix(v.hi);
                h.mix(v.lo);
            },
        });
    });
}

// The id of a field named in code. A missing name is a programming
// error (a renamed field, a typo), not data: it asserts in Debug and
// returns 0 — an id no reflected field carries — so a Release build
// skips the write instead of dereferencing null.
inline u32 fieldId(const TypeInfo& type, std::string_view name) {
    const FieldInfo* field = type.findField(name);
    ENGINE_ASSERT_MSG(field != nullptr, "{} has no field '{}'", type.name,
                      name);
    return field ? field->id : 0u;
}

} // namespace reflect
