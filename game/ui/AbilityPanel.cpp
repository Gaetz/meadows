#include "game/ui/AbilityPanel.hpp"

#include <imgui.h>

#include "engine/reflect/Reflect.hpp"
#include "game/ui/FormPicker.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/ability/GameplayEffects.hpp"

namespace game {

namespace {
constexpr ImVec4 kWarnColor { 1.0f, 0.6f, 0.2f, 1.0f };

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

} // namespace

void AbilityPanel::resetActor() {
    testSet = gameplay::AttributeSet {};
    testSystem = gameplay::AbilitySystem {};
    gameplay::initializeCurrent(testSystem, testSet);
    lastResult.clear();
}

void AbilityPanel::drawEditor(const core::Guid& abilityId) {
    const auto* type =
        abilityId.isValid() ? session.viewType(abilityId) : nullptr;
    if (!type || type->id != gameplay::AbilityForm::staticTypeInfo().id) {
        ImGui::TextDisabled("(select an ability in the Browser)");
        return;
    }
    const auto* ability =
        static_cast<const gameplay::AbilityForm*>(session.view(abilityId));

    ImGui::Text("%s", ability->editorId.c_str());
    ImGui::TextDisabled(
        "cost / cooldown / effect / conditions edit in the Inspector.");

    // Wiring health.
    const auto slot = [&](const char* name, const core::Guid& id) {
        if (!id.isValid()) {
            ImGui::TextDisabled("%s: (none)", name);
            return;
        }
        const auto* linked = static_cast<const gameplay::EffectForm*>(
            session.view(id));
        const auto* linkedType = session.viewType(id);
        if (!linked || !linkedType ||
            linkedType->id != gameplay::EffectForm::staticTypeInfo().id) {
            ImGui::TextColored(kWarnColor, "(!) %s: dangling guid", name);
            return;
        }
        ImGui::Text("%s: %s", name, linked->editorId.c_str());
        for (const str& warning : gameplay::effectWarnings(*linked)) {
            ImGui::TextColored(kWarnColor, "    (!) %s", warning.c_str());
        }
        if (str { name } == "cooldown" && linked->grantedTag.empty()) {
            ImGui::TextColored(kWarnColor,
                               "    (!) cooldown grants NO tag — there is "
                               "no effective cooldown");
        }
    };
    ImGui::SeparatorText("Wiring");
    slot("cost", ability->cost);
    slot("cooldown", ability->cooldown);
    slot("effect", ability->effect);
    if (!ability->script.empty()) {
        ImGui::TextDisabled("script: %s (runs on activation)",
                            ability->script.c_str());
    }

    // ---- Test activate: the REAL pipeline on a throwaway caster ------
    ImGui::SeparatorText("Test activate (throwaway caster, self-cast)");
    ImGui::TextDisabled(
        "resolves the RESOLVED database — export + reload to test "
        "session-created effects.");
    if (ImGui::Button("Reset actor")) {
        resetActor();
    }
    ImGui::SameLine();
    if (ImGui::Button("Try activate")) {
        if (testSystem.current.empty()) {
            resetActor();
        }
        // Register every tag the activation touches so gates resolve.
        const auto registerEffectTags = [&](const core::Guid& id) {
            if (const auto* effect =
                    forms.find<gameplay::EffectForm>(id)) {
                for (const str& tag :
                     { effect->grantedTag, effect->requiredTag,
                       effect->blockedTag }) {
                    if (!tag.empty()) {
                        testTags.registerTag(tag);
                    }
                }
            }
        };
        for (const str& tag : { ability->requiredTag, ability->blockedTag }) {
            if (!tag.empty()) {
                testTags.registerTag(tag);
            }
        }
        registerEffectTags(ability->cost);
        registerEffectTags(ability->cooldown);
        registerEffectTags(ability->effect);
        gameplay::grantAbility(testSystem, abilityId);
        const gameplay::AbilityContext ctx { forms, testTags };
        const bool activated =
            gameplay::tryActivate(*ability, testSet, testSystem, testSet,
                                  testSystem, ctx);
        lastResult = activated
                         ? "activated"
                         : "REFUSED (tags / cooldown / cost — see wiring)";
    }
    if (!lastResult.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s — %zu active effect(s)", lastResult.c_str(),
                            testSystem.activeEffects.size());
    }
    if (!testSystem.current.empty()) {
        drawAttributeTable(testSet, testSystem);
    } else {
        ImGui::TextDisabled("(Reset actor to build the test dummy)");
    }
}

} // namespace game
