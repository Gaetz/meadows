#pragma once

#include "data/plugins/EditSession.hpp"

namespace game {

// A combo that picks a form of one type by editorId, with a text filter
// (chantier 8.6 — first consumer: an anim state's `clip`). Sees exactly
// what the session sees (base + drafts, forEachVisible). Returns true
// when a pick was made this frame and writes it to `picked` — including
// "(none)", which yields the null guid (clears the field).
bool drawFormPicker(const char* label, data::EditSession& session,
                    u32 typeId, const core::Guid& current,
                    core::Guid& picked);

// The display name the picker shows for a guid: editorId, else the guid,
// else "(none)". Shared with node bodies that render resolved names.
str formDisplayName(const data::EditSession& session, const core::Guid& id);

} // namespace game
