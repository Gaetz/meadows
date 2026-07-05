#pragma once

#include "data/plugins/EditSession.hpp"

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

// Value <-> display helpers shared with the console.
str valueToString(const reflect::Value& value);
std::optional<reflect::Value> valueFromString(reflect::FieldKind kind,
                                              const str& text);

} // namespace game
