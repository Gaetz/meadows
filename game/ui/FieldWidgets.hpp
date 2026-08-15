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
// The raw core under textField, shared with the PropertyGrid: draws the
// InputText over the single active-edit cache; returns true when the
// interaction ends with an edit, with the final text in `edited`. The
// caller decides how to parse/commit it.
bool rawTextField(const core::Guid& id, u32 fieldId, const char* label,
                  const str& current, str& edited);
void floatField(data::EditSession& session, const core::Guid& id,
                const char* label, u32 fieldId, f32 current,
                f32 speed = 0.5f);

} // namespace game
