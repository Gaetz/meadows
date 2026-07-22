#pragma once

#include "engine/core/Defines.hpp"

namespace game {

// Closed keyword vocabularies of reflected string fields (nobody knows
// "Regular | Success | Failure" by heart — every
// keyword field gets a dropdown). Values are stored CANONICAL (exactly
// what the runtime parses: "greater", "useFurniture"…) and displayed
// Capitalized in blue, everywhere in the editor.
//
// The table is keyed by (type name, field name) — editor-side knowledge
// for now; if a validation tool needs it later, promote it next to the
// Forms (reflection metadata), not by duplicating the lists.

// The allowed values of a keyword field, or null when the field is free
// text. An empty-string entry means "unset" and renders as "(none)".
const vector<str>* keywordsFor(const str& typeName, const str& fieldName);

// Display form of a canonical keyword: first letter upper-cased
// ("useFurniture" -> "UseFurniture").
str keywordDisplay(const str& value);

// A combo over `options`, preview + items Capitalized and blue. Returns
// true when a pick was made this frame and writes the CANONICAL value.
bool drawKeywordCombo(const char* imguiLabel, const vector<str>& options,
                      const str& current, str& picked);

// Standalone blue keyword text (badges, labels).
void keywordText(const str& value);

} // namespace game
