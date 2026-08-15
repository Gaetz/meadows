#pragma once

#include "data/forms/Form.hpp"

// Animation data Forms. The anim RUNTIME lives in
// engine/anim (flat structs, no data:: dependency); world/scene/AnimBridge
// maps these Forms onto it. Variable-cardinality data (events, states,
// transitions) follows the CHILD-RECORD convention: children carry
// `parent` and are queried via data::childrenOf (FormQuery.hpp) — a mod
// adds an anim event or a graph state by adding one record.
//
// HOW TO FILL: the graph model is deliberately minimal
// (float params + tag gates). Add layers/masks fields by APPENDING.

namespace data {

class FormTypeRegistry;

// One clip: an animation inside a glTF asset.
struct AnimClipForm : Form {
    core::Guid asset;   // glTF file (VFS guid)
    str animationName;  // named animation inside the glTF ("" = first)
    f32 rate { 1.0f };
    bool loop { true };

    REFLECT_BEGIN(AnimClipForm, Form)
        REFLECT_FIELD(asset)
        REFLECT_FIELD(animationName)
        REFLECT_FIELD(rate)
        REFLECT_FIELD(loop)
    REFLECT_END()
};

// A tagged moment on a clip's timeline: hit frames (GAS damage windows),
// footsteps (audio by ground material), FX spawns. THE anim->gameplay
// bridge — the runtime fires `name` through a callback at `time`.
struct AnimEventForm : Form {
    core::Guid parent; // AnimClipForm
    f32 time { 0.0f }; // seconds, at rate 1
    str name;          // "Hit", "Footstep", "SpawnFx:Cue.Slash"...

    REFLECT_BEGIN(AnimEventForm, Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(time)
        REFLECT_FIELD(name)
    REFLECT_END()
};

// An animation controller: states + transitions, evaluated per entity.
// Parameters are floats (speed, posture...) plus gameplay-tag gates
// resolved through a callback — the graph never touches gameplay types.
struct AnimGraphForm : Form {
    core::Guid initialState; // AnimStateForm

    REFLECT_BEGIN(AnimGraphForm, Form)
        REFLECT_FIELD(initialState)
    REFLECT_END()
};

struct AnimStateForm : Form {
    core::Guid parent; // AnimGraphForm
    core::Guid clip;   // AnimClipForm
    f32 speed { 1.0f };
    // Anti-foot-sliding (in-place animation): playback rate is
    // scaled by (entity speed / referenceSpeed) when > 0.
    f32 referenceSpeed { 0.0f };

    REFLECT_BEGIN(AnimStateForm, Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(clip)
        REFLECT_FIELD(speed)
        REFLECT_FIELD(referenceSpeed)
    REFLECT_END()
};

// A transition between two states of the same graph. Fires when the float
// param crosses the threshold (compare: "greater" | "less") AND the
// optional tag gate passes. `blendTime` = cross-fade seconds.
struct AnimTransitionForm : Form {
    core::Guid parent; // AnimGraphForm
    core::Guid from;   // AnimStateForm (0 = any state)
    core::Guid to;     // AnimStateForm
    str param;         // float parameter name ("speed"...), "" = always
    str compare { "greater" };
    f32 threshold { 0.0f };
    str requiredTag;   // gameplay tag gate ("" = none)
    str blockedTag;
    f32 blendTime { 0.15f };
    bool waitForEnd { false }; // only fire when the clip finished (attacks)

    REFLECT_BEGIN(AnimTransitionForm, Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(from)
        REFLECT_FIELD(to)
        REFLECT_FIELD(param)
        REFLECT_FIELD(compare)
        REFLECT_FIELD(threshold)
        REFLECT_FIELD(requiredTag)
        REFLECT_FIELD(blockedTag)
        REFLECT_FIELD(blendTime)
        REFLECT_FIELD(waitForEnd)
    REFLECT_END()
};

void registerAnimFormTypes(FormTypeRegistry& registry);

} // namespace data
