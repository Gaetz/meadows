#include "game/ui/Keywords.hpp"

#include <cctype>

#include <imgui.h>

namespace game {

namespace {

// The keyword tint: keywords read Capitalized and blue.
constexpr ImVec4 kKeywordColor { 0.55f, 0.75f, 1.0f, 1.0f };

} // namespace

const reflect::EnumTable* keywordsFor(const reflect::TypeInfo& type,
                                      std::string_view fieldName) {
    const reflect::FieldInfo* field = type.findField(fieldName);
    return field ? field->enumeration : nullptr;
}

str keywordDisplay(std::string_view value) {
    str display { value };
    if (!display.empty()) {
        display[0] = static_cast<char>(
            std::toupper(static_cast<unsigned char>(display[0])));
    }
    return display;
}

bool drawKeywordCombo(const char* imguiLabel, const reflect::EnumTable& table,
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
    if (table.allowEmpty) { // "" = unset, gray "(none)"
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable("(none)", current.empty())) {
            picked = "";
            pickedAny = true;
        }
        ImGui::PopStyleColor();
    }
    for (const std::string_view option : table.names) {
        ImGui::PushStyleColor(ImGuiCol_Text, kKeywordColor);
        if (ImGui::Selectable(keywordDisplay(option).c_str(),
                              option == current)) {
            picked = str { option };
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
