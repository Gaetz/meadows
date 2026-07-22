#pragma once

#include <variant>

#include "engine/reflect/Reflect.hpp"

// One place to type-dispatch a reflect::Value. Every system that must act
// per-FieldKind (binary/TOML serialization, the editor property grid, the
// console, the Lua bridge) would otherwise hand-write the same 11-case `switch` with
// redundant std::get<> — and, crucially, WITHOUT a default, so adding a new
// FieldKind silently skipped a case and could corrupt a cooked plugin or save.
//
// `reflect::visit(value, overloaded{ ... })` replaces those switches. With one
// typed lambda per alternative (and NO generic `auto` catch-all), std::visit is
// EXHAUSTIVE: forgetting a kind is a compile error, not a runtime data bug — the
// safety net lands exactly on the §5 on-disk seam where it matters most.
//
// This is a mechanism only: each call site keeps its own per-kind body (writing
// bytes, drawing an ImGui widget, building a Lua object). Deliberately NOT a
// central table of bodies — that would force data/ to depend on ImGui and Lua
// and break the headless invariant (§2.10).

namespace reflect {

// The classic overload set for std::visit (not in the standard library).
template<typename... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template<typename... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

// Value first for readability at the call site.
template<typename Visitor>
decltype(auto) visit(const Value& value, Visitor&& visitor) {
    return std::visit(std::forward<Visitor>(visitor), value);
}

} // namespace reflect
