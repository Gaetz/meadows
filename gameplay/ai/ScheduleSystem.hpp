#pragma once

#include <optional>

#include "data/forms/FormDatabase.hpp"
#include "gameplay/ai/AiForms.hpp"
#include "gameplay/condition/Condition.hpp"

// Schedule evaluation (horizontal pass H7) — the WHEN/WHERE/WHAT layer of
// NPC daily life. Pure function of (database, schedule, game hour,
// conditions): headless, deterministic, doctested — the executing side
// (walking there, using the furniture) is the "vivant" vertical.
//
// Selection rule: among entries whose window contains the hour (windows
// wrap midnight when startHour > endHour) and whose ConditionForm clauses
// pass, the LAST one in load order wins — so a mod overrides a slice of
// anyone's day by adding one later entry (the child-record superpower).
//
// HOW TO FILL (post-7/07):
//  - a ScheduleAgent component per NPC caches the current intent and
//    re-evaluates on the game clock's hour change (not every frame);
//  - interruptions: combat/dialogue push their own intent, the schedule
//    resumes when they clear (intent stack);
//  - the debug view ("where is this NPC going and why") reads
//    ScheduleIntent::reason — keep it filled.

namespace gameplay {

struct ScheduleIntent {
    const AiPackageForm* package { nullptr };
    core::Guid location;   // entry override, else the package's location
    str reason;            // human-readable: entry editorId + window
};

std::optional<ScheduleIntent> evaluateSchedule(
    const data::FormDatabase& forms, const core::Guid& scheduleId,
    f32 hourOfDay, const EvalContext* conditions = nullptr);

} // namespace gameplay
