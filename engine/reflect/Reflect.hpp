#pragma once

// Subsystem map: docs/AUDIT/U1-foundations.md

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
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

// --- On-disk / patch contract lock ---
// A FieldKind ordinal is persisted as a raw u8 in cooked binaries and saves,
// and Value's alternative index IS that ordinal (see valueKind above).
// Reordering the enum, the variant, or these KindOf specializations would
// silently corrupt every existing cooked plugin and save. The asserts below
// pin the three in lockstep: any reorder/insert that desyncs them stops the
// build. To add a kind: append it LAST to the enum, the variant, and KindOf,
// bump the size assert, and add one kindMatches line.
template<FieldKind K, typename T>
inline constexpr bool kindMatches =
    std::is_same_v<std::variant_alternative_t<static_cast<std::size_t>(K), Value>, T> &&
    KindOf<T>::value == K;

static_assert(std::variant_size_v<Value> == 11,
              "FieldKind/Value count changed — append the new kind LAST and add "
              "its kindMatches assert (ordinals are an on-disk contract).");
static_assert(kindMatches<FieldKind::Bool, bool>);
static_assert(kindMatches<FieldKind::I32,  i32>);
static_assert(kindMatches<FieldKind::U32,  u32>);
static_assert(kindMatches<FieldKind::F32,  f32>);
static_assert(kindMatches<FieldKind::Str,  str>);
static_assert(kindMatches<FieldKind::Vec2, Vec2>);
static_assert(kindMatches<FieldKind::Vec3, Vec3>);
static_assert(kindMatches<FieldKind::Vec4, Vec4>);
static_assert(kindMatches<FieldKind::Quat, Quat>);
static_assert(kindMatches<FieldKind::Guid, core::Guid>);
static_assert(kindMatches<FieldKind::F64,  f64>);
} // namespace detail

enum FieldFlags : u32 {
    None = 0,
    // Excluded from serialization and patches (pure runtime state).
    Transient = 1u << 0,
    // A Guid field that names this form's OWNER: the FormDatabase buckets
    // the form under that guid (childrenOf). A field literally named
    // `parent` is a ParentLink without the flag — the child-record
    // convention; the flag is for owners under another name (a quest
    // branch's `state`, a task's `branch`).
    ParentLink = 1u << 1,
    // Still read for compatibility, no longer authored: the loader warns
    // once when a plugin writes it, the property grid hides it, the
    // validator counts the writes. Values keep resolving unchanged.
    Deprecated = 1u << 2,
};

// A closed vocabulary: the values a Str field may hold (or the ordinals
// an I32 field may hold), in the order of the C++ enum that consumes
// them. One table per vocabulary, declared next to that enum (or in
// data/forms/Vocabularies.hpp when the Form cannot see the enum), bound
// to the field by REFLECT_FIELD_ENUM. The loader rejects a value outside
// the table, the validator reports it, the property grid offers a
// dropdown — and the consumer parses with parseEnum. Zero effect on disk:
// the value stays the string.
struct EnumTable {
    std::span<const std::string_view> names;
    bool allowEmpty { false }; // "" = unset is a legal value

    constexpr size_t size() const { return names.size(); }
    constexpr std::string_view name(u32 ordinal) const {
        return ordinal < names.size() ? names[ordinal] : std::string_view {};
    }
    constexpr std::optional<u32> indexOf(std::string_view value) const {
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] == value) {
                return static_cast<u32>(i);
            }
        }
        return std::nullopt;
    }
    constexpr bool accepts(std::string_view value) const {
        return (allowEmpty && value.empty()) || indexOf(value).has_value();
    }
    constexpr bool accepts(i32 ordinal) const {
        return ordinal >= 0 && static_cast<size_t>(ordinal) < names.size();
    }
    // Str and I32 values are checked; any other kind passes.
    bool acceptsValue(const Value& value) const {
        if (const str* s = std::get_if<str>(&value)) {
            return accepts(std::string_view { *s });
        }
        if (const i32* i = std::get_if<i32>(&value)) {
            return accepts(*i);
        }
        return true;
    }
    // "a | b | c" for messages.
    str joined() const {
        str out;
        for (size_t i = 0; i < names.size(); ++i) {
            if (i) {
                out += " | ";
            }
            out += names[i];
        }
        return out;
    }
};

// The typed view of a table: E's enumerators are the table's ordinals.
template<typename E>
constexpr std::optional<E> parseEnum(const EnumTable& table,
                                     std::string_view value) {
    const auto index = table.indexOf(value);
    return index ? std::optional<E> { static_cast<E>(*index) } : std::nullopt;
}
template<typename E>
constexpr std::string_view enumName(const EnumTable& table, E value) {
    return table.name(static_cast<u32>(value));
}

struct FieldInfo {
    str name;
    u32 id { 0 }; // fnv1a(name) — stored in plugins/saves, so renaming
                  // a field breaks existing data (documented trade-off)
    FieldKind kind { FieldKind::Bool };
    u32 flags { None };
    const EnumTable* enumeration { nullptr }; // Str/I32 fields only

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
                            u32 flags = None,
                            const EnumTable* enumeration = nullptr) && {
        FieldInfo fieldInfo;
        fieldInfo.name = name;
        fieldInfo.id = core::fnv1a(name);
        fieldInfo.kind = detail::KindOf<F>::value;
        fieldInfo.flags = flags;
        fieldInfo.enumeration = enumeration;
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

// A Str (or ordinal I32) field bound to its closed vocabulary.
#define REFLECT_FIELD_ENUM(fieldName, enumTable)                              \
    .field(#fieldName, &ReflectedT::fieldName, ::reflect::None, &(enumTable))

#define REFLECT_END()                                                         \
                .build();                                                     \
        return reflectInfo;                                                   \
    }
