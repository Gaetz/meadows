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
//  - interruptions: DONE v1 (2026-07-13) — combat overrides frame by
//    frame (the director's dispatch already gives it the frame);
//    updateInterruption() below detects the edges so the executor stands
//    the NPC up on entry and forces an immediate re-evaluation on exit
//    (a fight spanning a slot boundary resumes on the CURRENT entry).
//    Dialogue joins when a reliable "dialogue open with X" signal exists
//    (QuestDirector::dialoguePartner is never cleared on close);
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

// Interruption edges (v1 of the promised intent hook): `interrupted` is
// the NPC's persistent flag, `busyNow` the frame's override (combat, later
// dialogue). Returns Interrupted exactly once on the rising edge (stand
// up: release furniture, drop the path) and Resumed exactly once on the
// falling edge (force an immediate schedule re-evaluation). Pure.
enum class ScheduleSignal { None, Interrupted, Resumed };
ScheduleSignal updateInterruption(bool& interrupted, bool busyNow);

} // namespace gameplay
