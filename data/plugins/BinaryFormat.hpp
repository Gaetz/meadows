#pragma once

#include <optional>
#include <span>

#include "data/plugins/Record.hpp"

namespace data {

// Cooked plugin format: little-endian (both target platforms are), magic
// 'MDWP' + format version, then the §5 record list with field values typed
// by FieldKind. A version bump may freely break the layout — this is a
// prototype format, the text form is the durable one.
//
// Determinism: fields are written sorted by field id, so cooking the same
// plugin twice yields byte-identical output.

inline constexpr u32 kPluginBinaryVersion = 1;

vector<u8> writePluginBinary(const Plugin& plugin);

// Bounds-checked; returns nullopt (with a logged error) on truncation, bad
// magic, or unknown version/field kind. Does not need the type registry:
// type/field ids are stored raw and validated later by the resolver.
std::optional<Plugin> readPluginBinary(std::span<const u8> bytes,
                                       std::string_view sourceName);

} // namespace data
