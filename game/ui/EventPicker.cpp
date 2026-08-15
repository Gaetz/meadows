#include "game/ui/EventPicker.hpp"

#include <algorithm>

#include <imgui.h>

#include "quest/Dialogue.hpp"
#include "quest/Quest.hpp"

namespace game {

namespace {

constexpr ImVec4 kEventColor { 0.55f, 0.75f, 1.0f, 1.0f }; // keyword blue

// Events dispatched from C++ (grep `dispatch({ eventKind(...)`) — they
// exist even when no record carries them yet. Extend when combat grows.
const vector<str> kBuiltinEvents { "OnDeath", "OnBanditDeath" };
// Events the C++ side LISTENS to (scene subscriptions) — a dialogue
// node firing one is not an orphan even with zero data listeners.
const vector<str> kBuiltinListeners { "OpenBarter", "OnPayFine" };

// The (type, field) pairs holding an EventBus event name. AnimEventForm
// `name` is NOT one: anim events fire through the anim runtime callback
// (cues channel), not the gameplay bus.
struct EventField {
    const char* type;
    const char* field;
};
const vector<EventField> kEventFields = {
    { "DialogueNodeForm", "event" }, // emitter (fired when entered/picked)
    { "QuestTaskForm", "event" },    // listener (progresses the task)
    { "QuestForm", "startEvent" },   // listener (starts the quest)
};

} // namespace

bool isEventField(const str& typeName, const str& fieldName) {
    return std::any_of(kEventFields.begin(), kEventFields.end(),
                       [&](const EventField& field) {
                           return typeName == field.type &&
                                  fieldName == field.field;
                       });
}

vector<str> collectEventNames(const data::EditSession& session) {
    vector<str> names = kBuiltinEvents;
    session.forEachVisible([&](const core::Guid&, const data::Form& form,
                               const reflect::TypeInfo& type) {
        for (const EventField& field : kEventFields) {
            if (type.name != field.type) {
                continue;
            }
            if (const reflect::FieldInfo* info =
                    type.findField(core::fnv1a(field.field))) {
                const reflect::Value value = info->get(&form);
                if (const str* name = std::get_if<str>(&value);
                    name && !name->empty()) {
                    names.push_back(*name);
                }
            }
        }
    });
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

bool drawEventCombo(const char* imguiLabel,
                    const data::EditSession& session, const str& current,
                    str& picked) {
    const str preview = current.empty() ? str { "(none)" } : current;
    ImGui::PushStyleColor(ImGuiCol_Text,
                          current.empty()
                              ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
                              : kEventColor);
    const bool open = ImGui::BeginCombo(imguiLabel, preview.c_str());
    ImGui::PopStyleColor();
    if (!open) {
        return false;
    }
    static char filter[64];
    if (ImGui::IsWindowAppearing()) {
        filter[0] = '\0';
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputTextWithHint("##evfilter", "filter or new name...", filter,
                             sizeof(filter));

    bool pickedAny = false;
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (ImGui::Selectable("(none)", current.empty())) {
        picked = "";
        pickedAny = true;
    }
    ImGui::PopStyleColor();

    const vector<str> names = collectEventNames(session);
    bool filterIsKnown = false;
    for (const str& name : names) {
        filterIsKnown = filterIsKnown || name == filter;
        if (filter[0] != '\0' && name.find(filter) == str::npos) {
            continue;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kEventColor);
        if (ImGui::Selectable((name + "##ev").c_str(), name == current)) {
            picked = name;
            pickedAny = true;
        }
        ImGui::PopStyleColor();
    }
    // A typed name that matches nothing becomes creatable — events exist
    // through use, this is where a NEW one enters the vocabulary.
    if (filter[0] != '\0' && !filterIsKnown) {
        const str label = str { "create \"" } + filter + "\"";
        if (ImGui::Selectable(label.c_str())) {
            picked = filter;
            pickedAny = true;
        }
    }
    ImGui::EndCombo();
    return pickedAny;
}

bool eventHasEmitter(const data::EditSession& session, const str& name) {
    if (std::find(kBuiltinEvents.begin(), kBuiltinEvents.end(), name) !=
        kBuiltinEvents.end()) {
        return true;
    }
    bool found = false;
    session.forEachVisible([&](const core::Guid&, const data::Form& form,
                               const reflect::TypeInfo& type) {
        if (found || type.name != "DialogueNodeForm") {
            return;
        }
        found = static_cast<const quest::DialogueNodeForm&>(form).event ==
                name;
    });
    return found;
}

bool eventHasListener(const data::EditSession& session, const str& name) {
    if (std::find(kBuiltinListeners.begin(), kBuiltinListeners.end(),
                  name) != kBuiltinListeners.end()) {
        return true;
    }
    bool found = false;
    session.forEachVisible([&](const core::Guid&, const data::Form& form,
                               const reflect::TypeInfo& type) {
        if (found) {
            return;
        }
        if (type.name == "QuestTaskForm") {
            found = static_cast<const quest::QuestTaskForm&>(form).event ==
                    name;
        } else if (type.name == "QuestForm") {
            found = static_cast<const quest::QuestForm&>(form).startEvent ==
                    name;
        }
    });
    return found;
}

namespace {

// The event a dialogue node fires — generated from its editorId the
// first time something wires to it ("On<EditorId>").
str ensureNodeEvent(data::EditSession& session, const core::Guid& nodeId) {
    const auto* node = static_cast<const quest::DialogueNodeForm*>(
        session.view(nodeId));
    if (!node) {
        return {}; // stale guid (node deleted since the list was built)
    }
    if (!node->event.empty()) {
        return node->event;
    }
    const str name =
        "On" + (node->editorId.empty() ? str { "Dialogue" } : node->editorId);
    session.setField(nodeId, core::fnv1a("event"), reflect::Value { name });
    return name;
}

// "quest / task" display label: task -> branch -> state -> quest chain.
str taskContextLabel(const data::EditSession& session,
                     const quest::QuestTaskForm& task) {
    str questName;
    if (const auto* branch = static_cast<const quest::QuestBranchForm*>(
            session.view(task.branch))) {
        if (const auto* state = static_cast<const quest::QuestStateForm*>(
                session.view(branch->state))) {
            if (const data::Form* quest = session.view(state->quest)) {
                questName = quest->editorId;
            }
        }
    }
    const str taskName =
        task.displayName.empty() ? task.editorId : task.displayName;
    return questName.empty() ? taskName : questName + " / " + taskName;
}

} // namespace

void drawEventWiring(data::EditSession& session, const core::Guid& target) {
    const reflect::TypeInfo* type =
        target.isValid() ? session.viewType(target) : nullptr;
    if (!type) {
        return;
    }
    static char filter[64];

    if (type->name == "DialogueNodeForm") {
        if (ImGui::Button("Wire to a quest task...")) {
            filter[0] = '\0';
            ImGui::OpenPopup("ev-wire-task");
        }
        if (ImGui::BeginPopup("ev-wire-task")) {
            ImGui::InputTextWithHint("##wf", "filter...", filter,
                                     sizeof(filter));
            vector<std::pair<str, core::Guid>> tasks;
            session.forEachVisible([&](const core::Guid& id,
                                       const data::Form& form,
                                       const reflect::TypeInfo& formType) {
                if (formType.name != "QuestTaskForm") {
                    return;
                }
                const str label = taskContextLabel(
                    session,
                    static_cast<const quest::QuestTaskForm&>(form));
                if (filter[0] != '\0' && label.find(filter) == str::npos) {
                    return;
                }
                tasks.emplace_back(label, id);
            });
            std::sort(tasks.begin(), tasks.end());
            for (const auto& [label, id] : tasks) {
                if (ImGui::Selectable(
                        (label + "##wt" + id.toString()).c_str())) {
                    data::EditSession::Gesture gesture { session };
                    const str eventName = ensureNodeEvent(session, target);
                    session.setField(id, core::fnv1a("event"),
                                     reflect::Value { eventName });
                }
            }
            if (tasks.empty()) {
                ImGui::TextDisabled("(no quest task yet)");
            }
            ImGui::EndPopup();
        }
        return;
    }

    if (type->name == "QuestTaskForm") {
        if (ImGui::Button("Wire to a dialogue option...")) {
            filter[0] = '\0';
            ImGui::OpenPopup("ev-wire-node");
        }
        if (ImGui::BeginPopup("ev-wire-node")) {
            ImGui::InputTextWithHint("##wf", "filter...", filter,
                                     sizeof(filter));
            vector<std::pair<str, core::Guid>> nodes;
            session.forEachVisible([&](const core::Guid& id,
                                       const data::Form& form,
                                       const reflect::TypeInfo& formType) {
                if (formType.name != "DialogueNodeForm") {
                    return;
                }
                const auto& node =
                    static_cast<const quest::DialogueNodeForm&>(form);
                str label = node.speaker.empty() ? "(npc)" : node.speaker;
                label += ": ";
                label += node.text.size() > 40 ? node.text.substr(0, 40) + "..."
                                               : node.text;
                if (!node.event.empty()) {
                    label += "  [" + node.event + "]";
                }
                if (filter[0] != '\0' && label.find(filter) == str::npos) {
                    return;
                }
                nodes.emplace_back(label, id);
            });
            std::sort(nodes.begin(), nodes.end());
            for (const auto& [label, id] : nodes) {
                if (ImGui::Selectable(
                        (label + "##wn" + id.toString()).c_str())) {
                    data::EditSession::Gesture gesture { session };
                    const str eventName = ensureNodeEvent(session, id);
                    session.setField(target, core::fnv1a("event"),
                                     reflect::Value { eventName });
                }
            }
            if (nodes.empty()) {
                ImGui::TextDisabled("(no dialogue line yet)");
            }
            ImGui::EndPopup();
        }
    }
}

void drawEventCrossRef(const data::EditSession& session,
                       const core::Guid& targetIn, core::Guid& selected) {
    // Copy first: callers may pass `selected` itself as the target, and a
    // click mutates it mid-scan.
    const core::Guid target = targetIn;
    const reflect::TypeInfo* type =
        target.isValid() ? session.viewType(target) : nullptr;
    const data::Form* form = target.isValid() ? session.view(target) : nullptr;
    if (!type || !form) {
        return;
    }
    // The record's event name, whichever of the known fields it carries.
    str eventName;
    for (const EventField& field : kEventFields) {
        if (type->name != field.type) {
            continue;
        }
        if (const reflect::FieldInfo* info =
                type->findField(core::fnv1a(field.field))) {
            const reflect::Value value = info->get(form);
            if (const str* name = std::get_if<str>(&value)) {
                eventName = *name;
            }
        }
    }
    if (eventName.empty()) {
        return;
    }

    ImGui::SeparatorText(("Event: " + eventName).c_str());
    u32 hits = 0;
    session.forEachVisible([&](const core::Guid& id, const data::Form& other,
                               const reflect::TypeInfo& otherType) {
        if (id == target) {
            return;
        }
        const auto entry = [&](const char* role, const str& detail) {
            const str label = str { role } + "  " +
                              (other.editorId.empty() ? id.toString()
                                                      : other.editorId) +
                              (detail.empty() ? "" : "  — " + detail) +
                              "##xr" + id.toString();
            if (ImGui::Selectable(label.c_str(), selected == id)) {
                selected = id;
            }
            ++hits;
        };
        if (otherType.name == "DialogueNodeForm") {
            const auto* node =
                static_cast<const quest::DialogueNodeForm*>(&other);
            if (node->event == eventName) {
                entry("fired by",
                      node->text.size() > 32 ? node->text.substr(0, 32) + "..."
                                             : node->text);
            }
        } else if (otherType.name == "QuestTaskForm") {
            const auto* task = static_cast<const quest::QuestTaskForm*>(&other);
            if (task->event == eventName) {
                entry("progresses", task->displayName);
            }
        } else if (otherType.name == "QuestForm") {
            const auto* quest = static_cast<const quest::QuestForm*>(&other);
            if (quest->startEvent == eventName) {
                entry("starts", quest->displayName);
            }
        }
    });
    if (std::find(kBuiltinEvents.begin(), kBuiltinEvents.end(), eventName) !=
        kBuiltinEvents.end()) {
        ImGui::TextDisabled("also fired by the game (C++)");
        ++hits;
    }
    if (hits == 0) {
        ImGui::TextDisabled("(nothing else references this event)");
    }
}

} // namespace game
