#include "gameplay/ai/ScheduleSystem.hpp"

#include "data/forms/FormQuery.hpp"

namespace gameplay {

namespace {

bool windowContains(f32 startHour, f32 endHour, f32 hour) {
    if (startHour <= endHour) {
        return hour >= startHour && hour < endHour;
    }
    // Wraps midnight: 22 -> 6 means [22, 24) U [0, 6).
    return hour >= startHour || hour < endHour;
}

} // namespace

std::optional<ScheduleIntent> evaluateSchedule(
    const data::FormDatabase& forms, const core::Guid& scheduleId,
    f32 hourOfDay, const EvalContext* conditions) {
    const ScheduleEntryForm* winner = nullptr;
    // childrenOf visits in load order: keeping the LAST match implements
    // "a later plugin's entry overrides the slice".
    data::childrenOf<ScheduleEntryForm>(
        forms, scheduleId, [&](const ScheduleEntryForm& entry) {
            if (!windowContains(entry.startHour, entry.endHour,
                                hourOfDay)) {
                return;
            }
            if (conditions &&
                !conditionsPass(forms, entry.id, *conditions)) {
                return;
            }
            winner = &entry;
        });
    if (!winner) {
        return std::nullopt;
    }
    const auto* package = forms.find<AiPackageForm>(winner->package);
    if (!package) {
        return std::nullopt;
    }
    ScheduleIntent intent;
    intent.package = package;
    intent.location = winner->location.isValid() ? winner->location
                                                 : package->location;
    intent.reason = winner->editorId + " [" +
                    std::to_string(winner->startHour) + "h-" +
                    std::to_string(winner->endHour) + "h] -> " +
                    package->kind;
    return intent;
}

ScheduleSignal updateInterruption(bool& interrupted, bool busyNow) {
    if (busyNow && !interrupted) {
        interrupted = true;
        return ScheduleSignal::Interrupted;
    }
    if (!busyNow && interrupted) {
        interrupted = false;
        return ScheduleSignal::Resumed;
    }
    return ScheduleSignal::None;
}

} // namespace gameplay
