#pragma once

#include <string_view>

#include <imgui.h>

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"

namespace game {

// The editor's shared warning tint (orange) — panels flag missing
// wiring / dangling refs with it.
inline constexpr ImVec4 kWarnColor { 1.0f, 0.6f, 0.2f, 1.0f };

// Keyword fields = reflected string fields bound to a reflect::EnumTable
// (REFLECT_FIELD_ENUM): the SAME table the loader and the validator
// check, so nothing is listed twice. Values are stored CANONICAL (what
// the runtime parses: "greater", "useFurniture") and displayed
// Capitalized in blue, everywhere in the editor.

// The vocabulary of a field, or null when it is free text.
const reflect::EnumTable* keywordsFor(const reflect::TypeInfo& type,
                                      std::string_view fieldName);

// Display form of a canonical keyword: first letter upper-cased
// ("useFurniture" -> "UseFurniture").
str keywordDisplay(std::string_view value);

// A combo over the table (plus "(none)" when it allows empty), preview +
// items Capitalized and blue. Returns true when a pick was made this
// frame and writes the CANONICAL value.
bool drawKeywordCombo(const char* imguiLabel, const reflect::EnumTable& table,
                      const str& current, str& picked);

// Standalone blue keyword text (badges, labels).
void keywordText(const str& value);

} // namespace game
