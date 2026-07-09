#include "game/ui/Keywords.hpp"

#include <cctype>

#include <imgui.h>

namespace game {

namespace {

// The keyword tint (dev-decided): keywords read Capitalized and blue.
constexpr ImVec4 kKeywordColor { 0.55f, 0.75f, 1.0f, 1.0f };

struct Vocab {
    const char* type;
    const char* field;
    vector<str> options;
};

// The status-buildup channels (docs/STATS.md §3 / MODDING-EFFECTS) —
// shared by every `buildupType` field. "" = no buildup.
const vector<str> kBuildupTypes {
    "", "poison", "bleed", "mental", "disease", "curse",
    "death", "ignition", "glaciation", "electrocution",
};

// One entry per closed vocabulary. Canonical values ONLY — these strings
// are what the C++ parsers switch on; the display capitalization is
// cosmetic (keywordDisplay).
const vector<Vocab> kVocabs = {
    { "QuestStateForm", "kind", { "Regular", "Success", "Failure" } },
    { "ConditionForm", "kind",
      { "HasTag", "AttributeAtLeast", "AttributeAtMost", "HasItem", "Lua" } },
    { "AnimTransitionForm", "compare", { "greater", "less" } },
    { "EffectForm", "op", { "add", "multiply", "override" } },
    { "EffectForm", "duration",
      { "instant", "duration", "infinite", "periodic" } },
    { "EffectForm", "expiryMode", { "immediate", "decay" } },
    { "EffectForm", "buildupType", kBuildupTypes },
    { "WeaponForm", "buildupType", kBuildupTypes },
    { "AbilityForm", "costPolicy", { "", "permissive", "strict" } },
    { "AiPackageForm", "kind",
      { "sleep", "eat", "work", "wander", "travel", "useFurniture",
        "guard" } },
    { "ArmorForm", "slot", { "head", "torso", "arms", "legs" } },
};

} // namespace

const vector<str>* keywordsFor(const str& typeName, const str& fieldName) {
    for (const Vocab& vocab : kVocabs) {
        if (typeName == vocab.type && fieldName == vocab.field) {
            return &vocab.options;
        }
    }
    return nullptr;
}

str keywordDisplay(const str& value) {
    str display = value;
    if (!display.empty()) {
        display[0] = static_cast<char>(
            std::toupper(static_cast<unsigned char>(display[0])));
    }
    return display;
}

bool drawKeywordCombo(const char* imguiLabel, const vector<str>& options,
                      const str& current, str& picked) {
    const str preview =
        current.empty() ? str { "(none)" } : keywordDisplay(current);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          current.empty()
                              ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
                              : kKeywordColor);
    const bool open = ImGui::BeginCombo(imguiLabel, preview.c_str());
    ImGui::PopStyleColor();
    if (!open) {
        return false;
    }
    bool pickedAny = false;
    for (const str& option : options) {
        if (option.empty()) { // "" = unset, gray "(none)"
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::GetStyleColorVec4(
                                      ImGuiCol_TextDisabled));
            if (ImGui::Selectable("(none)", current.empty())) {
                picked = "";
                pickedAny = true;
            }
            ImGui::PopStyleColor();
            continue;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kKeywordColor);
        if (ImGui::Selectable(keywordDisplay(option).c_str(),
                              option == current)) {
            picked = option;
            pickedAny = true;
        }
        ImGui::PopStyleColor();
    }
    ImGui::EndCombo();
    return pickedAny;
}

void keywordText(const str& value) {
    ImGui::TextColored(kKeywordColor, "%s", keywordDisplay(value).c_str());
}

} // namespace game
