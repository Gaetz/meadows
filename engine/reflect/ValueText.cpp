#include "engine/reflect/ValueText.hpp"

#include <array>
#include <sstream>

#include "engine/reflect/Visit.hpp"

namespace reflect {

str valueToString(const Value& value) {
    // Round-trippable text (re-parsed by valueFromString): raw string, space-
    // separated vector components, no decoration. Exhaustive per kind.
    std::ostringstream out;
    visit(value, overloaded {
        [&](bool b)              { out << (b ? "true" : "false"); },
        [&](i32 x)               { out << x; },
        [&](u32 x)               { out << x; },
        [&](f32 x)               { out << x; },
        [&](f64 x)               { out << x; },
        [&](const str& s)        { out << s; },
        [&](const Vec2& v)       { out << v.x << " " << v.y; },
        [&](const Vec3& v)       { out << v.x << " " << v.y << " " << v.z; },
        [&](const Vec4& v)       { out << v.x << " " << v.y << " " << v.z << " " << v.w; },
        [&](const Quat& q)       { out << q.x << " " << q.y << " " << q.z << " " << q.w; },
        [&](const core::Guid& g) { out << g.toString(); },
    });
    return out.str();
}

std::optional<Value> valueFromString(FieldKind kind, const str& text) {
    const auto floats = [&](u32 count) -> std::optional<std::array<f32, 4>> {
        std::istringstream in { text };
        std::array<f32, 4> out {};
        for (u32 i = 0; i < count; ++i) {
            if (!(in >> out[i])) {
                return std::nullopt;
            }
        }
        return out;
    };
    switch (kind) {
    case FieldKind::Bool:
        return Value { text == "true" || text == "1" };
    case FieldKind::I32:
        try { return Value { static_cast<i32>(std::stol(text)) }; }
        catch (...) { return std::nullopt; }
    case FieldKind::U32:
        try { return Value { static_cast<u32>(std::stoul(text)) }; }
        catch (...) { return std::nullopt; }
    case FieldKind::F32:
        try { return Value { std::stof(text) }; }
        catch (...) { return std::nullopt; }
    case FieldKind::F64:
        try { return Value { std::stod(text) }; }
        catch (...) { return std::nullopt; }
    case FieldKind::Str:
        return Value { text };
    case FieldKind::Guid:
        if (const auto guid = core::Guid::fromString(text)) {
            return Value { *guid };
        }
        return std::nullopt;
    case FieldKind::Vec2:
        if (const auto v = floats(2)) {
            return Value { Vec2 { (*v)[0], (*v)[1] } };
        }
        return std::nullopt;
    case FieldKind::Vec3:
        if (const auto v = floats(3)) {
            return Value { Vec3 { (*v)[0], (*v)[1], (*v)[2] } };
        }
        return std::nullopt;
    case FieldKind::Vec4:
        if (const auto v = floats(4)) {
            return Value { Vec4 { (*v)[0], (*v)[1], (*v)[2], (*v)[3] } };
        }
        return std::nullopt;
    case FieldKind::Quat:
        if (const auto v = floats(4)) {
            Quat q;
            q.x = (*v)[0]; q.y = (*v)[1]; q.z = (*v)[2]; q.w = (*v)[3];
            return Value { q };
        }
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace reflect
