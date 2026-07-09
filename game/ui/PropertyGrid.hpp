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

// The Value <-> text codec moved to engine/reflect/ValueText (U4-11: the
// CSV importer shares it); these usings keep the console/grid call sites.
using reflect::valueFromString;
using reflect::valueToString;

} // namespace game
