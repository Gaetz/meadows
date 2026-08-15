#include "game/ui/TestActor.hpp"

#include <imgui.h>

#include "engine/reflect/Reflect.hpp"

namespace game {

void TestActor::reset() {
    set = gameplay::AttributeSet {};
    system = gameplay::AbilitySystem {};
    gameplay::initializeCurrent(system, set);
    lastResult.clear();
}

void drawAttributeTable(const gameplay::AttributeSet& set,
                        const gameplay::AbilitySystem& system) {
    if (!ImGui::BeginTable("attrs", 3, ImGuiTableFlags_SizingStretchProp)) {
        return;
    }
    ImGui::TableSetupColumn("attribute");
    ImGui::TableSetupColumn("base");
    ImGui::TableSetupColumn("current");
    ImGui::TableHeadersRow();
    reflect::forEachField(
        gameplay::AttributeSet::staticTypeInfo(),
        [&](const reflect::FieldInfo& field) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(field.name.c_str());
            ImGui::TableNextColumn();
            const auto base =
                gameplay::baseValueOf(set, gameplay::attr(field.name));
            ImGui::Text("%.1f", base ? *base : 0.0f);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", gameplay::currentValueOf(
                                    system, gameplay::attr(field.name)));
        });
    ImGui::EndTable();
}

} // namespace game
