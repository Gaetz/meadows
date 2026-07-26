#pragma once

// Subsystem map: docs/AUDIT/U1-foundations.md

#include <functional>
#include <unordered_map>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/core/Defines.hpp"

// The animation seam (docs/HORIZONTAL-PASS.md): a pure, headless runtime over
// PLAIN structs — no data:: dependency (rule: engine consumes params, the
// world/runtime layer maps Forms onto them via world/scene/AnimBridge).
// Deterministic by construction (same clip + times = same pose), so it
// doctests without a renderer, like the rest of the sim.
//
// NO root motion — the character controller owns
// movement; GraphState::referenceSpeed rescales playback against entity
// speed to hide foot sliding.
//
// Planned extension points:
//  - GPU skinning: skinMatrices() output -> bone-palette SSBO (the RHI
//    compute/storage extension already exists), skinned vertex shader;
//  - layers/masks: add an upper-body mask blend on top of blendPose
//    (blend per joint with a weight array — the data model can grow an
//    AnimMaskForm then);
//  - events -> gameplay: route GraphInstance's event callback into the
//    EventBus / GAS hit windows (AnimEventForm names are the contract).

namespace anim {

struct Joint {
    str name;
    i32 parent { -1 }; // index into Skeleton::joints, -1 = root
    Mat4 inverseBind { 1.0f };
    Vec3 bindPosition { 0.0f };
    Quat bindRotation { 1.0f, 0.0f, 0.0f, 0.0f };
    Vec3 bindScale { 1.0f };
};

struct Skeleton {
    vector<Joint> joints; // parents always before children
    i32 findJoint(std::string_view name) const;
};

struct JointPose {
    Vec3 position { 0.0f };
    Quat rotation { 1.0f, 0.0f, 0.0f, 0.0f };
    Vec3 scale { 1.0f };
};
using Pose = vector<JointPose>;

// Per-joint keyframe channels; empty channel = keep the pose's current
// value (clips may animate a subset of joints).
struct JointTrack {
    vector<f32> positionTimes;
    vector<Vec3> positions;
    vector<f32> rotationTimes;
    vector<Quat> rotations;
    vector<f32> scaleTimes;
    vector<Vec3> scales;
};

struct AnimClip {
    str name;
    f32 duration { 0.0f };
    vector<JointTrack> tracks; // indexed like Skeleton::joints
};

void bindPose(const Skeleton& skeleton, Pose& out);
// Samples animated channels over `inOut` (call bindPose first, or blend
// over another sampled pose). `time` is clamped or wrapped by the caller.
void samplePose(const AnimClip& clip, f32 time, Pose& inOut);
void blendPose(const Pose& from, const Pose& to, f32 alpha, Pose& out);
// Local poses -> model-space joint matrices.
void modelMatrices(const Skeleton& skeleton, const Pose& pose,
                   vector<Mat4>& out);
// Model matrices x inverse bind: the GPU bone palette.
void skinMatrices(const Skeleton& skeleton, const Pose& pose,
                  vector<Mat4>& out);

// --- Graph runtime -------------------------------------------------------------

struct AnimEvent {
    f32 time { 0.0f };
    str name;
};

struct GraphState {
    u32 clip { 0 }; // index into GraphDesc::clips
    f32 speed { 1.0f };
    bool loop { true };
    f32 referenceSpeed { 0.0f }; // > 0: playback *= entitySpeed / this
};

struct GraphTransition {
    i32 from { -1 }; // state index, -1 = any state
    u32 to { 0 };
    str param;       // "" = unconditional (still gated by tags/waitForEnd)
    bool greater { true };
    f32 threshold { 0.0f };
    str requiredTag;
    str blockedTag;
    f32 blendTime { 0.15f };
    bool waitForEnd { false };
    // Full condition-evaluator gate: an opaque id the
    // runtime hands back through the condition callback — the graph never
    // sees gameplay types, same seam as tags. "" = ungated (the common
    // case pays no callback). Fails closed when set without a callback.
    str conditionRef {};
};

struct GraphDesc {
    vector<AnimClip> clips;
    vector<vector<AnimEvent>> clipEvents; // parallel to clips
    vector<GraphState> states;
    vector<GraphTransition> transitions;
    u32 initialState { 0 };
};

// One animated entity. Owns current/next state + cross-fade; parameters
// are floats, tag gates go through a callback (the gameplay layer plugs
// its TagContainer in — the graph never sees gameplay types).
class GraphInstance {
public:
    using TagCheck = std::function<bool(std::string_view tag)>;
    using ConditionCheck = std::function<bool(std::string_view conditionRef)>;
    using EventSink = std::function<void(std::string_view name)>;

    explicit GraphInstance(const GraphDesc& desc);

    void setParam(std::string_view name, f32 value);
    f32 param(std::string_view name) const;
    void setTagCheck(TagCheck check) { tagCheck = std::move(check); }
    void setConditionCheck(ConditionCheck check) {
        conditionCheck = std::move(check);
    }
    void setEventSink(EventSink sink) { eventSink = std::move(sink); }

    // Advances time (firing crossed events), evaluates transitions, and
    // progresses the cross-fade. `entitySpeed` feeds referenceSpeed sync.
    void update(f32 dt, f32 entitySpeed = 0.0f);

    // Writes the current (possibly blended) pose over `inOut`.
    void evaluate(Pose& inOut) const;

    u32 currentState() const { return current; }
    bool blending() const { return blendRemaining > 0.0f; }

private:
    f32 playbackRate(const GraphState& state, f32 entitySpeed) const;
    void advanceTime(f32 dt, f32 entitySpeed);
    void checkTransitions();
    void startTransition(const GraphTransition& transition);

    const GraphDesc& desc;
    u32 current { 0 };
    f32 currentTime { 0.0f };
    u32 next { 0 };            // valid while blending
    f32 nextTime { 0.0f };
    f32 blendRemaining { 0.0f };
    f32 blendDuration { 0.0f };
    std::unordered_map<str, f32> params;
    TagCheck tagCheck;
    ConditionCheck conditionCheck;
    EventSink eventSink;
};

} // namespace anim
