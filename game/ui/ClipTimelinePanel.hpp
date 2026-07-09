#pragma once

#include "data/plugins/EditSession.hpp"

namespace game {

// The anim-clip event timeline (chantier 8.8): AnimEventForm children of
// the selected AnimClipForm on a seconds strip — the anim->gameplay
// bridge (hit frames, footsteps, FX spawns) edited like the schedule
// timeline: live preview while dragging, ONE field edit on release
// (0.01 s snap), "+ event" on an empty spot. The clip's real duration
// lives in the glTF (not the Form): the view length is derived from the
// events (max time + margin) and adjustable manually. 8.7b contract:
// the shell owns the windows — drawEditor fills the Editor center.
class ClipTimelinePanel {
public:
    ClipTimelinePanel(data::EditSession& session, core::Guid& selected)
        : session { session }, selected { selected } {}

    void drawEditor(const core::Guid& clip);

private:
    data::EditSession& session;
    core::Guid& selected;

    f32 viewLength { 0.0f };  // 0 = auto (max event time + margin)
    core::Guid dragEvent;     // marker being dragged
    f32 dragTime { 0.0f };    // live preview value (0.01 s snap)
    u32 createCounter { 0 };
};

} // namespace game
