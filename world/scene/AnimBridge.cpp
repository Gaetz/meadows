#include "world/scene/AnimBridge.hpp"

#include <unordered_map>

#include "data/forms/AnimForms.hpp"
#include "data/forms/FormQuery.hpp"
#include "engine/core/Log.hpp"

namespace world {

std::optional<anim::GraphDesc> buildAnimGraph(
    const data::FormDatabase& forms, const core::Guid& graphId,
    const ClipResolver& resolveClip) {
    const auto* graph = forms.find<data::AnimGraphForm>(graphId);
    if (!graph) {
        LOG_ERROR("AnimBridge: no AnimGraphForm {}", graphId.toString());
        return std::nullopt;
    }

    anim::GraphDesc desc;
    // State guid -> runtime index, filled as states resolve; clips dedup
    // by AnimClipForm guid (several states may share one clip).
    std::unordered_map<core::Guid, u32> stateIndex;
    std::unordered_map<core::Guid, u32> clipIndex;

    const auto clipFor = [&](const core::Guid& clipId)
        -> std::optional<u32> {
        if (const auto it = clipIndex.find(clipId);
            it != clipIndex.end()) {
            return it->second;
        }
        const auto* clipForm = forms.find<data::AnimClipForm>(clipId);
        if (!clipForm) {
            return std::nullopt;
        }
        auto clip = resolveClip(clipForm->asset, clipForm->animationName);
        if (!clip) {
            return std::nullopt;
        }
        // Clip-level rate bakes into duration-preserving playback speed at
        // the state level; events come from child records.
        const u32 index = static_cast<u32>(desc.clips.size());
        desc.clips.push_back(std::move(*clip));
        desc.clipEvents.emplace_back();
        data::childrenOf<data::AnimEventForm>(
            forms, clipId, [&](const data::AnimEventForm& event) {
                desc.clipEvents.back().push_back(
                    { event.time, event.name });
            });
        clipIndex.emplace(clipId, index);
        return index;
    };

    data::childrenOf<data::AnimStateForm>(
        forms, graphId, [&](const data::AnimStateForm& state) {
            const auto clip = clipFor(state.clip);
            if (!clip) {
                LOG_WARN("AnimBridge: state '{}' skipped (clip missing)",
                         state.editorId);
                return;
            }
            const auto* clipForm =
                forms.find<data::AnimClipForm>(state.clip);
            anim::GraphState runtime;
            runtime.clip = *clip;
            runtime.speed = state.speed * (clipForm ? clipForm->rate : 1.0f);
            runtime.loop = clipForm ? clipForm->loop : true;
            runtime.referenceSpeed = state.referenceSpeed;
            stateIndex.emplace(state.id,
                               static_cast<u32>(desc.states.size()));
            desc.states.push_back(runtime);
        });
    if (desc.states.empty()) {
        LOG_ERROR("AnimBridge: graph '{}' has no usable states",
                  graph->editorId);
        return std::nullopt;
    }

    data::childrenOf<data::AnimTransitionForm>(
        forms, graphId, [&](const data::AnimTransitionForm& transition) {
            const auto to = stateIndex.find(transition.to);
            if (to == stateIndex.end()) {
                return;
            }
            anim::GraphTransition runtime;
            runtime.from = -1;
            if (const auto from = stateIndex.find(transition.from);
                from != stateIndex.end()) {
                runtime.from = static_cast<i32>(from->second);
            }
            runtime.to = to->second;
            runtime.param = transition.param;
            runtime.greater = transition.compare != "less";
            runtime.threshold = transition.threshold;
            runtime.requiredTag = transition.requiredTag;
            runtime.blockedTag = transition.blockedTag;
            runtime.blendTime = transition.blendTime;
            runtime.waitForEnd = transition.waitForEnd;
            desc.transitions.push_back(std::move(runtime));
        });

    if (const auto initial = stateIndex.find(graph->initialState);
        initial != stateIndex.end()) {
        desc.initialState = initial->second;
    }
    return desc;
}

} // namespace world
