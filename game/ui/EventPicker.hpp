#pragma once

#include "data/plugins/EditSession.hpp"

namespace game {

// The gameplay-event vocabulary of the editor (chantier 8.7c). Events
// are NAMES that exist only through use: a dialogue node fires one, a
// quest task listens for one, a quest starts on one (startEvent), and a
// few come from C++ (combat). The editor makes that articulation
// VISIBLE: event fields get a dropdown fed by scanning the session for
// every name in use (plus the known C++-emitted ones), with inline
// creation for new names; and the Inspector shows who emits / listens /
// starts on the selected record's event, with click navigation.

// True for the string fields that hold an EventBus event name.
bool isEventField(const str& typeName, const str& fieldName);

// Every event name visible to the session (emitters + listeners +
// startEvents + the C++-emitted builtins), sorted, unique.
vector<str> collectEventNames(const data::EditSession& session);

// Combo over the known names + "create '<typed>'" for new ones. Returns
// true when a pick was made this frame and writes it to `picked`.
bool drawEventCombo(const char* imguiLabel,
                    const data::EditSession& session, const str& current,
                    str& picked);

// If `target` is a record with a non-empty event field, lists the
// cross-references of that event (fired by / progresses / starts),
// clickable into `selected`. No-op otherwise.
void drawEventCrossRef(const data::EditSession& session,
                       const core::Guid& target, core::Guid& selected);

// Orphan checks (8.7d, the lint side of the cross-ref): does anything
// FIRE this name (a dialogue node or the C++ emitters), does anything
// REACT to it (a task, a quest startEvent, or the C++ listeners)?
bool eventHasEmitter(const data::EditSession& session, const str& name);
bool eventHasListener(const data::EditSession& session, const str& name);

// The explicit wiring gesture (dev feedback on 8.7d — "how do I create
// an event from the dialogue to the quest?"): on a dialogue node, "Wire
// to a quest task..." picks a task and gives BOTH sides the same event
// (generated from the node's editorId when missing); on a quest task,
// "Wire to a dialogue option..." does the mirror. No-op on other types.
void drawEventWiring(data::EditSession& session, const core::Guid& target);

} // namespace game
