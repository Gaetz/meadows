#pragma once

#include "data/plugins/EditSession.hpp"

namespace game {

// THE shared condition widget — dialogue options and
// abilities all speak ConditionForm (ANDed clauses hung by `parent`).
// Lists the parent's clauses as summaries (gameplay::conditionSummary);
// the selected clause expands into CONTEXTUAL editors — only the fields
// its `kind` reads (tag / attribute+value / item picker+count / lua) —
// plus negate. "+ condition" pre-fills the parent; delete only removes
// session drafts (§5). Every edit is an ordinary EditSession op.
void drawConditionList(data::EditSession& session, const core::Guid& parent,
                       core::Guid& selected);

} // namespace game
