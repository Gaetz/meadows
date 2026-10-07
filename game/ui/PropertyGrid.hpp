#pragma once

#include "data/plugins/EditSession.hpp"
#include "engine/reflect/ValueText.hpp"

namespace game {

// THE reflection-driven editor widget (§2.3: reflection powers the
// property panels). Draws every non-transient reflected field of the form
// as the right ImGui widget for its FieldKind, and commits completed edits
// through the EditSession (one undo step per finished interaction, not per
// drag tick). Reused by the GameDB browser and, later, the level editor's
// reference inspector — extend HERE, not per-panel.
//
// Returns true when a field was committed this frame.
bool drawPropertyGrid(data::EditSession& session, const core::Guid& id);

// The same grid over a PLAIN reflected object (no EditSession, no
// undo): the tuning pupitres edit a live struct and commit through the
// reflected setter when the widget deactivates. Fields whose name
// starts with `filter` (case-insensitive, empty = all) are shown.
// Returns true when a field changed this frame.
bool drawReflectedStruct(void* object, const reflect::TypeInfo& type,
                         const char* filter = "");

// The Value <-> text codec lives in engine/reflect/ValueText (the
// CSV importer shares it); these usings keep the console/grid call sites.
using reflect::valueFromString;
using reflect::valueToString;

} // namespace game
