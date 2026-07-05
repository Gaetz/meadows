#include "engine/anim/Anim.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace anim {

namespace {

// Keyframe interpolation: index of the key at or before `time`, plus the
// normalized fraction toward the next key.
template<typename T>
T sampleChannel(const vector<f32>& times, const vector<T>& values, f32 time,
                const T& fallback) {
    if (times.empty()) {
        return fallback;
    }
    if (time <= times.front()) {
        return values.front();
    }
    if (time >= times.back()) {
        return values.back();
    }
    const auto upper = std::upper_bound(times.begin(), times.end(), time);
    const size_t index = static_cast<size_t>(upper - times.begin()) - 1;
    const f32 span = times[index + 1] - times[index];
    const f32 alpha = span > 1e-8f ? (time - times[index]) / span : 0.0f;
    if constexpr (std::is_same_v<T, Quat>) {
        return glm::slerp(values[index], values[index + 1], alpha);
    } else {
        return glm::mix(values[index], values[index + 1], alpha);
    }
}

} // namespace

i32 Skeleton::findJoint(std::string_view name) const {
    for (size_t i = 0; i < joints.size(); ++i) {
        if (joints[i].name == name) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

void bindPose(const Skeleton& skeleton, Pose& out) {
    out.resize(skeleton.joints.size());
    for (size_t i = 0; i < skeleton.joints.size(); ++i) {
        out[i] = { skeleton.joints[i].bindPosition,
                   skeleton.joints[i].bindRotation,
                   skeleton.joints[i].bindScale };
    }
}

void samplePose(const AnimClip& clip, f32 time, Pose& inOut) {
    const size_t joints = std::min(inOut.size(), clip.tracks.size());
    for (size_t i = 0; i < joints; ++i) {
        const JointTrack& track = clip.tracks[i];
        if (!track.positions.empty()) {
            inOut[i].position = sampleChannel(track.positionTimes,
                                              track.positions, time,
                                              inOut[i].position);
        }
        if (!track.rotations.empty()) {
            inOut[i].rotation = sampleChannel(track.rotationTimes,
                                              track.rotations, time,
                                              inOut[i].rotation);
        }
        if (!track.scales.empty()) {
            inOut[i].scale = sampleChannel(track.scaleTimes, track.scales,
                                           time, inOut[i].scale);
        }
    }
}

void blendPose(const Pose& from, const Pose& to, f32 alpha, Pose& out) {
    const size_t joints = std::min(from.size(), to.size());
    out.resize(joints);
    for (size_t i = 0; i < joints; ++i) {
        out[i].position = glm::mix(from[i].position, to[i].position, alpha);
        out[i].rotation = glm::slerp(from[i].rotation, to[i].rotation,
                                     alpha);
        out[i].scale = glm::mix(from[i].scale, to[i].scale, alpha);
    }
}

void modelMatrices(const Skeleton& skeleton, const Pose& pose,
                   vector<Mat4>& out) {
    out.resize(skeleton.joints.size());
    for (size_t i = 0; i < skeleton.joints.size(); ++i) {
        Mat4 local = glm::translate(Mat4 { 1.0f }, pose[i].position) *
                     glm::mat4_cast(pose[i].rotation) *
                     glm::scale(Mat4 { 1.0f }, pose[i].scale);
        const i32 parent = skeleton.joints[i].parent;
        // Parents precede children: out[parent] is already final.
        out[i] = parent >= 0 ? out[parent] * local : local;
    }
}

void skinMatrices(const Skeleton& skeleton, const Pose& pose,
                  vector<Mat4>& out) {
    modelMatrices(skeleton, pose, out);
    for (size_t i = 0; i < skeleton.joints.size(); ++i) {
        out[i] = out[i] * skeleton.joints[i].inverseBind;
    }
}

// --- GraphInstance --------------------------------------------------------------

GraphInstance::GraphInstance(const GraphDesc& desc)
    : desc { desc }, current { desc.initialState } {}

void GraphInstance::setParam(std::string_view name, f32 value) {
    params[str { name }] = value;
}

f32 GraphInstance::param(std::string_view name) const {
    const auto it = params.find(str { name });
    return it != params.end() ? it->second : 0.0f;
}

f32 GraphInstance::playbackRate(const GraphState& state,
                                f32 entitySpeed) const {
    f32 rate = state.speed;
    if (state.referenceSpeed > 0.0f && entitySpeed > 0.0f) {
        rate *= entitySpeed / state.referenceSpeed; // anti foot-sliding
    }
    return rate;
}

void GraphInstance::advanceTime(f32 dt, f32 entitySpeed) {
    const auto advance = [&](u32 stateIndex, f32& time, bool fireEvents) {
        const GraphState& state = desc.states[stateIndex];
        const AnimClip& clip = desc.clips[state.clip];
        const f32 previous = time;
        time += dt * playbackRate(state, entitySpeed);
        if (fireEvents && eventSink &&
            state.clip < desc.clipEvents.size()) {
            for (const AnimEvent& event : desc.clipEvents[state.clip]) {
                // Crossed this frame (loop wrap handled below by reset).
                if (event.time > previous && event.time <= time) {
                    eventSink(event.name);
                }
            }
        }
        if (clip.duration > 0.0f && time >= clip.duration) {
            time = state.loop ? std::fmod(time, clip.duration)
                              : clip.duration;
        }
    };
    advance(current, currentTime, true);
    if (blendRemaining > 0.0f) {
        advance(next, nextTime, false);
        blendRemaining -= dt;
        if (blendRemaining <= 0.0f) {
            current = next;
            currentTime = nextTime;
            blendRemaining = 0.0f;
        }
    }
}

void GraphInstance::checkTransitions() {
    if (blendRemaining > 0.0f) {
        return; // one transition at a time
    }
    for (const GraphTransition& transition : desc.transitions) {
        if (transition.from >= 0 &&
            static_cast<u32>(transition.from) != current) {
            continue;
        }
        if (transition.to == current) {
            continue;
        }
        if (transition.waitForEnd) {
            const GraphState& state = desc.states[current];
            const AnimClip& clip = desc.clips[state.clip];
            if (state.loop || currentTime < clip.duration) {
                continue;
            }
        }
        if (!transition.param.empty()) {
            const f32 value = param(transition.param);
            const bool pass = transition.greater
                                  ? value > transition.threshold
                                  : value < transition.threshold;
            if (!pass) {
                continue;
            }
        }
        if (!transition.requiredTag.empty() &&
            (!tagCheck || !tagCheck(transition.requiredTag))) {
            continue;
        }
        if (!transition.blockedTag.empty() && tagCheck &&
            tagCheck(transition.blockedTag)) {
            continue;
        }
        startTransition(transition);
        return;
    }
}

void GraphInstance::startTransition(const GraphTransition& transition) {
    if (transition.blendTime <= 0.0f) {
        current = transition.to;
        currentTime = 0.0f;
        return;
    }
    next = transition.to;
    nextTime = 0.0f;
    blendRemaining = transition.blendTime;
    blendDuration = transition.blendTime;
}

void GraphInstance::update(f32 dt, f32 entitySpeed) {
    if (desc.states.empty()) {
        return;
    }
    advanceTime(dt, entitySpeed);
    checkTransitions();
}

void GraphInstance::evaluate(Pose& inOut) const {
    if (desc.states.empty()) {
        return;
    }
    const GraphState& state = desc.states[current];
    samplePose(desc.clips[state.clip], currentTime, inOut);
    if (blendRemaining > 0.0f && blendDuration > 0.0f) {
        Pose target = inOut;
        samplePose(desc.clips[desc.states[next].clip], nextTime, target);
        const f32 alpha = 1.0f - blendRemaining / blendDuration;
        blendPose(inOut, target, alpha, inOut);
    }
}

} // namespace anim
