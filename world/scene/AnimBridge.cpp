#include "world/scene/AnimBridge.hpp"

#include <algorithm>
#include <tuple>
#include <unordered_map>

#include "data/forms/AnimForms.hpp"
#include "data/forms/CoreForms.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/plugins/EditSession.hpp"
#include "engine/core/Log.hpp"
#include "gameplay/actors/CharacterForms.hpp"
#include "gameplay/condition/Condition.hpp"

namespace world {

namespace {

// The Form->GraphDesc mapping, parameterized over HOW forms are looked up:
// the resolved-database path (runtime) and the EditSession path (anim
// preview — unsaved drafts included) build through this ONE mapping.
struct GraphForms {
    std::function<const data::AnimGraphForm*(const core::Guid&)> graph;
    std::function<const data::AnimClipForm*(const core::Guid&)> clip;
    std::function<vector<const data::AnimEventForm*>(const core::Guid&)>
        events;
    std::function<vector<const data::AnimStateForm*>(const core::Guid&)>
        states;
    std::function<vector<const data::AnimTransitionForm*>(const core::Guid&)>
        transitions;
    // Whether a transition record carries ConditionForm children (the §2.11
    // shared predicate engine): decided at BUILD time so ungated
    // transitions never pay the runtime condition callback.
    std::function<bool(const core::Guid&)> hasConditions;
};

std::optional<anim::GraphDesc> buildFromForms(const GraphForms& source,
                                              const core::Guid& graphId,
                                              const ClipResolver& resolveClip) {
    const data::AnimGraphForm* graph = source.graph(graphId);
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
        const data::AnimClipForm* clipForm = source.clip(clipId);
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
        for (const data::AnimEventForm* event : source.events(clipId)) {
            desc.clipEvents.back().push_back({ event->time, event->name });
        }
        clipIndex.emplace(clipId, index);
        return index;
    };

    for (const data::AnimStateForm* state : source.states(graphId)) {
        const auto clip = clipFor(state->clip);
        if (!clip) {
            LOG_WARN("AnimBridge: state '{}' skipped (clip missing)",
                     state->editorId);
            continue;
        }
        const data::AnimClipForm* clipForm = source.clip(state->clip);
        anim::GraphState runtime;
        runtime.clip = *clip;
        runtime.speed = state->speed * (clipForm ? clipForm->rate : 1.0f);
        runtime.loop = clipForm ? clipForm->loop : true;
        runtime.referenceSpeed = state->referenceSpeed;
        stateIndex.emplace(state->id, static_cast<u32>(desc.states.size()));
        desc.states.push_back(runtime);
    }
    if (desc.states.empty()) {
        LOG_ERROR("AnimBridge: graph '{}' has no usable states",
                  graph->editorId);
        return std::nullopt;
    }

    for (const data::AnimTransitionForm* transition :
         source.transitions(graphId)) {
        const auto to = stateIndex.find(transition->to);
        if (to == stateIndex.end()) {
            continue;
        }
        anim::GraphTransition runtime;
        runtime.from = -1;
        if (const auto from = stateIndex.find(transition->from);
            from != stateIndex.end()) {
            runtime.from = static_cast<i32>(from->second);
        }
        runtime.to = to->second;
        runtime.param = transition->param;
        runtime.greater = transition->compare != "less";
        runtime.threshold = transition->threshold;
        runtime.requiredTag = transition->requiredTag;
        runtime.blockedTag = transition->blockedTag;
        runtime.blendTime = transition->blendTime;
        runtime.waitForEnd = transition->waitForEnd;
        if (source.hasConditions(transition->id)) {
            // The opaque ref handed back through the condition callback IS
            // the transition's guid: conditionsPass evaluates its
            // ConditionForm children (the dialogue/quest/ability pattern).
            runtime.conditionRef = transition->id.toString();
        }
        desc.transitions.push_back(std::move(runtime));
    }

    if (const auto initial = stateIndex.find(graph->initialState);
        initial != stateIndex.end()) {
        desc.initialState = initial->second;
    }
    return desc;
}

// EditSession lookups: view() serves drafts over base forms; children come
// from forEachVisible so session-CREATED records participate before export.
template <typename T>
const T* viewAs(const data::EditSession& session, const core::Guid& id) {
    const reflect::TypeInfo* type = session.viewType(id);
    if (!type || !type->isA(T::staticTypeInfo().id)) {
        return nullptr;
    }
    return static_cast<const T*>(session.view(id));
}

template <typename T>
vector<const T*> visibleChildren(const data::EditSession& session,
                                 const core::Guid& parent) {
    vector<const T*> base;
    vector<const T*> created;
    session.forEachVisible([&](const core::Guid& id, const data::Form& form,
                               const reflect::TypeInfo& type) {
        if (!type.isA(T::staticTypeInfo().id)) {
            return;
        }
        const T& child = static_cast<const T&>(form);
        if (child.parent != parent) {
            return;
        }
        (session.isCreated(id) ? created : base).push_back(&child);
    });
    std::sort(created.begin(), created.end(), [](const T* a, const T* b) {
        return std::tie(a->editorId, a->id) < std::tie(b->editorId, b->id);
    });
    base.insert(base.end(), created.begin(), created.end());
    return base;
}

} // namespace

std::optional<anim::GraphDesc> buildAnimGraph(
    const data::FormDatabase& forms, const core::Guid& graphId,
    const ClipResolver& resolveClip) {
    const GraphForms source {
        .graph = [&](const core::Guid& id) {
            return forms.find<data::AnimGraphForm>(id);
        },
        .clip = [&](const core::Guid& id) {
            return forms.find<data::AnimClipForm>(id);
        },
        .events = [&](const core::Guid& id) {
            return data::collectChildren<data::AnimEventForm>(forms, id);
        },
        .states = [&](const core::Guid& id) {
            return data::collectChildren<data::AnimStateForm>(forms, id);
        },
        .transitions = [&](const core::Guid& id) {
            return data::collectChildren<data::AnimTransitionForm>(forms, id);
        },
        .hasConditions = [&](const core::Guid& id) {
            return !data::collectChildren<gameplay::ConditionForm>(forms, id)
                        .empty();
        },
    };
    return buildFromForms(source, graphId, resolveClip);
}

std::optional<anim::GraphDesc> buildAnimGraph(
    const data::EditSession& session, const core::Guid& graphId,
    const ClipResolver& resolveClip) {
    const GraphForms source {
        .graph = [&](const core::Guid& id) {
            return viewAs<data::AnimGraphForm>(session, id);
        },
        .clip = [&](const core::Guid& id) {
            return viewAs<data::AnimClipForm>(session, id);
        },
        .events = [&](const core::Guid& id) {
            return visibleChildren<data::AnimEventForm>(session, id);
        },
        .states = [&](const core::Guid& id) {
            return visibleChildren<data::AnimStateForm>(session, id);
        },
        .transitions = [&](const core::Guid& id) {
            return visibleChildren<data::AnimTransitionForm>(session, id);
        },
        .hasConditions = [&](const core::Guid& id) {
            return !visibleChildren<gameplay::ConditionForm>(session, id)
                        .empty();
        },
    };
    return buildFromForms(source, graphId, resolveClip);
}

std::optional<ActorVisual> resolveActorVisual(const data::FormDatabase& forms,
                                              const data::ActorForm& actor) {
    if (!actor.appearance.isValid()) {
        return std::nullopt; // 2D/legacy actor — sprite path only
    }
    const auto* appearance =
        forms.find<gameplay::AppearanceForm>(actor.appearance);
    if (!appearance || !appearance->skeleton.isValid()) {
        LOG_WARN("resolveActorVisual: actor '{}' has no usable appearance",
                 actor.editorId);
        return std::nullopt;
    }
    // First filled slot = the body mesh (v1 single-mesh characters).
    const core::Guid slots[] = { appearance->torsoMesh, appearance->headMesh,
                                 appearance->legsMesh, appearance->hairMesh,
                                 appearance->handsMesh,
                                 appearance->feetMesh };
    ActorVisual visual;
    visual.skeleton = appearance->skeleton;
    for (const core::Guid& slot : slots) {
        if (slot.isValid()) {
            visual.mesh = slot;
            break;
        }
    }
    if (!visual.mesh.isValid()) {
        return std::nullopt;
    }
    visual.tint = appearance->skinTint;
    visual.animGraph = actor.animGraph;
    return visual;
}

} // namespace world
