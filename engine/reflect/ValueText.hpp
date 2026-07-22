#pragma once

#include <optional>

#include "engine/reflect/Reflect.hpp"

namespace reflect {

// Round-trippable text form of a reflected Value (raw string, space-
// separated vector components, no decoration) — the SHARED string codec
// for the property grid, the dev console and the CSV importer. The
// exhaustive-visit guarantee (reflect::visit) applies:
// a new FieldKind breaks the build here, not silently one consumer.
str valueToString(const Value& value);
std::optional<Value> valueFromString(FieldKind kind, const str& text);

} // namespace reflect
