#include "game/ui/AbilityPanel.hpp"
#include "game/ui/Keywords.hpp"

#include <imgui.h>

#include "game/ui/TestActor.hpp"

#include "engine/reflect/Reflect.hpp"
#include "game/ui/FormPicker.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/ability/GameplayEffects.hpp"

namespace game {

namespace {


} // namespace

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
        testActor.reset();
    }
    ImGui::SameLine();
    if (ImGui::Button("Try activate")) {
        if (testActor.system.current.empty()) {
            testActor.reset();
        }
        // Register every tag the activation touches so gates resolve.
        const auto registerEffectTags = [&](const core::Guid& id) {
            if (const auto* effect =
                    forms.find<gameplay::EffectForm>(id)) {
                for (const str& tag :
                     { effect->grantedTag, effect->requiredTag,
                       effect->blockedTag }) {
                    if (!tag.empty()) {
                        testActor.tags.registerTag(tag);
                    }
                }
            }
        };
        for (const str& tag : { ability->requiredTag, ability->blockedTag }) {
            if (!tag.empty()) {
                testActor.tags.registerTag(tag);
            }
        }
        registerEffectTags(ability->cost);
        registerEffectTags(ability->cooldown);
        registerEffectTags(ability->effect);
        gameplay::grantAbility(testActor.system, abilityId);
        const gameplay::AbilityContext ctx { forms, testActor.tags };
        const bool activated =
            gameplay::tryActivate(*ability, testActor.set, testActor.system, testActor.set,
                                  testActor.system, ctx);
        testActor.lastResult = activated
                         ? "activated"
                         : "REFUSED (tags / cooldown / cost — see wiring)";
    }
    if (!testActor.lastResult.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s — %zu active effect(s)", testActor.lastResult.c_str(),
                            testActor.system.activeEffects.size());
    }
    if (!testActor.system.current.empty()) {
        drawAttributeTable(testActor.set, testActor.system);
    } else {
        ImGui::TextDisabled("(Reset actor to build the test dummy)");
    }
}

} // namespace game
