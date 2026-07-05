#pragma once

#include <functional>
#include <string_view>
#include <variant>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"
#include "engine/core/Hash.hpp"

// The reflection system (§2.3): every Form type and component declares its
// fields once, in its own header, through the REFLECT_* macros. This single
// registration powers serialization, the field-level patch system, saves,
// and editor property panels — never write per-type serialization code.
//
// Usage (all in the header, inside the struct):
//
//   struct WeaponForm : Form {
//       f32 damage { 10.0f };
//       str displayName;
//
//       REFLECT_BEGIN(WeaponForm, Form)
//           REFLECT_FIELD(damage)
//           REFLECT_FIELD(displayName)
//       REFLECT_END()
//   };
//
// Root types use REFLECT_BEGIN(Type, void).

namespace reflect {

// Closed set of field types. Extend it (enum entry + Value alternative, same
// order) when a concrete Form needs more; unsupported field types fail to
// compile. Containers/nested structs are deliberately out of v1: when they
// arrive, a container patches as one atomic value (whole-field replace).
enum class FieldKind : u8 {
    Bool,
    I32,
    U32,
    F32,
    Str,
    Vec2,
    Vec3,
    Vec4,
    Quat,
    Guid,
    F64, // appended last so existing kind ordinals (persisted in cooked binary)
         // stay stable
};

// Type-erased field value: the currency of serialization and patches.
// Alternative order must match FieldKind.
using Value = std::variant<bool, i32, u32, f32, str, Vec2, Vec3, Vec4, Quat,
                           core::Guid, f64>;

constexpr FieldKind valueKind(const Value& value) {
    return static_cast<FieldKind>(value.index());
}

namespace detail {
template<typename T>
struct KindOf;
template<> struct KindOf<bool> { static constexpr auto value = FieldKind::Bool; };
template<> struct KindOf<i32>  { static constexpr auto value = FieldKind::I32; };
template<> struct KindOf<u32>  { static constexpr auto value = FieldKind::U32; };
template<> struct KindOf<f32>  { static constexpr auto value = FieldKind::F32; };
template<> struct KindOf<f64>  { static constexpr auto value = FieldKind::F64; };
template<> struct KindOf<str>  { static constexpr auto value = FieldKind::Str; };
template<> struct KindOf<Vec2> { static constexpr auto value = FieldKind::Vec2; };
template<> struct KindOf<Vec3> { static constexpr auto value = FieldKind::Vec3; };
template<> struct KindOf<Vec4> { static constexpr auto value = FieldKind::Vec4; };
template<> struct KindOf<Quat> { static constexpr auto value = FieldKind::Quat; };
template<> struct KindOf<core::Guid> { static constexpr auto value = FieldKind::Guid; };
} // namespace detail

enum FieldFlags : u32 {
    None = 0,
    // Excluded from serialization and patches (pure runtime state).
    Transient = 1u << 0,
};

struct FieldInfo {
    str name;
    u32 id { 0 }; // fnv1a(name) — stored in plugins/saves, so renaming
                  // a field breaks existing data (documented trade-off)
    FieldKind kind { FieldKind::Bool };
    u32 flags { None };

    // Type-erased access; `object` must point to an instance of the
    // reflected type (or a derived one).
    std::function<Value(const void* object)> get;
    // Returns false (and writes nothing) if the value's kind mismatches —
    // mod data is untrusted, the caller logs with context and skips (§8:
    // recoverable error, no exception).
    std::function<bool(void* object, const Value& value)> set;
};

struct TypeInfo {
    str name;
    u32 id { 0 }; // fnv1a(name), stable across runs and platforms
    u32 size { 0 };
    const TypeInfo* parent { nullptr };
    vector<FieldInfo> fields; // own fields only; lookups walk parents

    const FieldInfo* findField(u32 fieldId) const {
        for (const FieldInfo& field : fields) {
            if (field.id == fieldId) {
                return &field;
            }
        }
        return parent ? parent->findField(fieldId) : nullptr;
    }
    const FieldInfo* findField(std::string_view fieldName) const {
        return findField(core::fnv1a(fieldName));
    }

    bool isA(u32 typeId) const {
        return id == typeId || (parent && parent->isA(typeId));
    }
};

// Visits every field including inherited ones, parents first (the natural
// display/serialization order). The editor property grid, clone and diff
// paths all iterate through this — one traversal to rule them out of sync.
template<typename Fn>
void forEachField(const TypeInfo& type, Fn&& fn) {
    if (type.parent) {
        forEachField(*type.parent, fn);
    }
    for (const FieldInfo& field : type.fields) {
        fn(field);
    }
}

// Resolves the parent argument of REFLECT_BEGIN; void = root type.
template<typename T>
const TypeInfo* typeInfoOf() {
    return &T::staticTypeInfo();
}
template<>
inline const TypeInfo* typeInfoOf<void>() {
    return nullptr;
}

template<typename T>
class TypeInfoBuilder {
public:
    TypeInfoBuilder(const char* name, const TypeInfo* parent) {
        info.name = name;
        info.id = core::fnv1a(name);
        info.size = sizeof(T);
        info.parent = parent;
    }

    template<typename F>
    TypeInfoBuilder&& field(const char* name, F T::* member,
                            u32 flags = None) && {
        FieldInfo fieldInfo;
        fieldInfo.name = name;
        fieldInfo.id = core::fnv1a(name);
        fieldInfo.kind = detail::KindOf<F>::value;
        fieldInfo.flags = flags;
        fieldInfo.get = [member](const void* object) -> Value {
            return Value { static_cast<const T*>(object)->*member };
        };
        fieldInfo.set = [member](void* object, const Value& value) -> bool {
            const F* typed = std::get_if<F>(&value);
            if (!typed) {
                return false;
            }
            static_cast<T*>(object)->*member = *typed;
            return true;
        };
        info.fields.push_back(std::move(fieldInfo));
        return std::move(*this);
    }

    TypeInfo build() && { return std::move(info); }

private:
    TypeInfo info;
};

} // namespace reflect

#define REFLECT_BEGIN(Type, Parent)                                           \
    static const ::reflect::TypeInfo& staticTypeInfo() {                      \
        using ReflectedT = Type;                                              \
        static const ::reflect::TypeInfo reflectInfo =                        \
            ::reflect::TypeInfoBuilder<ReflectedT> {                          \
                #Type, ::reflect::typeInfoOf<Parent>()                        \
            }

#define REFLECT_FIELD(fieldName) .field(#fieldName, &ReflectedT::fieldName)

#define REFLECT_FIELD_FLAGS(fieldName, fieldFlags)                            \
    .field(#fieldName, &ReflectedT::fieldName, fieldFlags)

#define REFLECT_END()                                                         \
                .build();                                                     \
        return reflectInfo;                                                   \
    }
