#pragma once

#include "data/plugins/EditSession.hpp"

namespace game {

// Tiny commit-on-deactivate field widgets over ONE reflected field
// (factored out of the condition builder): the structured
// panels (conditions, effects) edit a handful of fields contextually
// without rebuilding the PropertyGrid. One undo step per finished
// interaction, same active-edit discipline as the grid.
void textField(data::EditSession& session, const core::Guid& id,
               const char* label, u32 fieldId, const str& current);
void floatField(data::EditSession& session, const core::Guid& id,
                const char* label, u32 fieldId, f32 current,
                f32 speed = 0.5f);

} // namespace game
