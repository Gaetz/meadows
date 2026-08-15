#include "game/ui/FormPicker.hpp"

#include <algorithm>

#include <imgui.h>

#include "data/forms/AnimForms.hpp"
#include "data/forms/AudioForms.hpp"
#include "data/forms/VisualForms.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ai/AiForms.hpp"

namespace game {

str formDisplayName(const data::EditSession& session, const core::Guid& id) {
    if (!id.isValid()) {
        return "(none)";
    }
    const data::Form* form = session.view(id);
    if (!form) {
        return id.toString() + " (missing)";
    }
    return form->editorId.empty() ? id.toString() : form->editorId;
}

const reflect::TypeInfo* pickerTypeFor(const str& typeName,
                                       const str& fieldName) {
    if (typeName == "AnimStateForm" && fieldName == "clip") {
        return &data::AnimClipForm::staticTypeInfo();
    }
    if (typeName == "CueForm" && fieldName == "particles") {
        return &data::ParticleForm::staticTypeInfo();
    }
    if (typeName == "CueForm" && fieldName == "sound") {
        return &data::SoundForm::staticTypeInfo();
    }
    if (typeName == "AbilityForm" &&
        (fieldName == "cost" || fieldName == "cooldown" ||
         fieldName == "effect")) {
        return &gameplay::EffectForm::staticTypeInfo();
    }
    if (typeName == "ScheduleEntryForm" && fieldName == "package") {
        return &gameplay::AiPackageForm::staticTypeInfo();
    }
    return nullptr;
}

bool isItemField(const str& typeName, const str& fieldName) {
    return (typeName == "DialogueNodeForm" && fieldName == "takeItem") ||
           (typeName == "QuestForm" && fieldName == "rewardItem") ||
           (typeName == "ConditionForm" && fieldName == "item");
}

namespace {

// Shared skeleton of both pickers: filterable combo, "(none)" entry,
// entries collected then sorted by name — forEachVisible's draft order is
// a hash order, and a flickering combo is unusable. One shared filter
// buffer is fine: ImGui opens a single combo at a time.
template <typename AcceptsType>
bool drawPickerCombo(const char* label, data::EditSession& session,
                     const core::Guid& current, core::Guid& picked,
                     AcceptsType&& acceptsType, bool withTypeSuffix) {
    const str preview = formDisplayName(session, current);
    bool pickedAny = false;
    if (!ImGui::BeginCombo(label, preview.c_str())) {
        return false;
    }
    static char filter[64];
    if (ImGui::IsWindowAppearing()) {
        filter[0] = '\0';
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputText("##pickerfilter", filter, sizeof(filter));
    if (ImGui::Selectable("(none)", !current.isValid())) {
        picked = {};
        pickedAny = true;
    }
    vector<std::pair<str, core::Guid>> entries;
    session.forEachVisible([&](const core::Guid& id, const data::Form& form,
                               const reflect::TypeInfo& type) {
        if (!acceptsType(type)) {
            return;
        }
        str name = form.editorId.empty() ? id.toString() : form.editorId;
        if (withTypeSuffix) {
            name += "  (" + type.name + ")";
        }
        if (filter[0] != '\0' && name.find(filter) == str::npos) {
            return;
        }
        entries.emplace_back(std::move(name), id);
    });
    std::sort(entries.begin(), entries.end());
    for (const auto& [name, id] : entries) {
        if (ImGui::Selectable((name + "##pk" + id.toString()).c_str(),
                              id == current)) {
            picked = id;
            pickedAny = true;
        }
    }
    ImGui::EndCombo();
    return pickedAny;
}

} // namespace

bool drawItemPicker(const char* label, data::EditSession& session,
                    const core::Guid& current, core::Guid& picked) {
    return drawPickerCombo(
        label, session, current, picked,
        [](const reflect::TypeInfo& type) {
            return type.name == "WeaponForm" || type.name == "ArmorForm" ||
                   type.name == "ConsumableForm" ||
                   type.name == "MiscItemForm";
        },
        true);
}

bool drawFormPicker(const char* label, data::EditSession& session,
                    u32 typeId, const core::Guid& current,
                    core::Guid& picked) {
    return drawPickerCombo(
        label, session, current, picked,
        [typeId](const reflect::TypeInfo& type) {
            return type.id == typeId;
        },
        false);
}

} // namespace game
