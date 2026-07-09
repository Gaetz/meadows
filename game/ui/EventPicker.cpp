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
