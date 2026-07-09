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

// 8.7e: ITEM guid fields (dialogue takeItem, quest rewardItem, condition
// item) span the four item categories — the picker lists them all,
// labelled "editorId (Type)". Same contract as drawFormPicker.
bool isItemField(const str& typeName, const str& fieldName);
bool drawItemPicker(const char* label, data::EditSession& session,
                    const core::Guid& current, core::Guid& picked);

} // namespace game
