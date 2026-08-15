#include "game/ui/EffectPanel.hpp"

#include <imgui.h>

#include "game/ui/TestActor.hpp"

#include "engine/reflect/Reflect.hpp"
#include "game/ui/FieldWidgets.hpp"
#include "game/ui/Keywords.hpp"
#include "gameplay/ability/GameplayEffects.hpp"

namespace game {

namespace {

// The test actor's attributes, base vs current, straight from the
// AttributeSet reflection (transient `damage` included on purpose: the
// meta-attribute route is exactly what a damage effect exercises).

} // namespace

void EffectPanel::drawEditor(const core::Guid& effectId) {
    const auto* type =
        effectId.isValid() ? session.viewType(effectId) : nullptr;
    if (!type || type->id != gameplay::EffectForm::staticTypeInfo().id) {
        ImGui::TextDisabled("(select an effect in the Browser)");
        return;
    }
    const auto* effect =
        static_cast<const gameplay::EffectForm*>(session.view(effectId));

    ImGui::Text("%s", effect->editorId.c_str());
    for (const str& warning : gameplay::effectWarnings(*effect)) {
        ImGui::TextColored(kWarnColor, "(!) %s", warning.c_str());
    }

    const auto keywordCombo = [&](const char* label, const char* fieldName,
                                  const str& current) {
        const auto* options = keywordsFor("EffectForm", fieldName);
        str picked;
        if (options &&
            drawKeywordCombo(label, *options, current, picked)) {
            session.setField(effectId, core::fnv1a(fieldName),
                             reflect::Value { picked });
        }
    };

    ImGui::SeparatorText("Modifier");
    textField(session, effectId, "attribute", core::fnv1a("attribute"),
              effect->attribute);
    keywordCombo("op", "op", effect->op);
    floatField(session, effectId, "magnitude", core::fnv1a("magnitude"),
               effect->magnitude);
    if (ImGui::TreeNode("second modifier")) {
        textField(session, effectId, "attribute2",
                  core::fnv1a("attribute2"), effect->attribute2);
        floatField(session, effectId, "magnitude2",
                   core::fnv1a("magnitude2"), effect->magnitude2);
        ImGui::TreePop();
    }

    ImGui::SeparatorText("Duration");
    keywordCombo("kind", "duration", effect->duration);
    if (effect->duration == "duration" || effect->duration == "periodic" ||
        effect->duration == "infinite") {
        if (effect->duration != "infinite") {
            floatField(session, effectId, "seconds (real time)",
                       core::fnv1a("durationSeconds"),
                       effect->durationSeconds, 0.1f);
            floatField(session, effectId, "hours (game time)",
                       core::fnv1a("durationHours"), effect->durationHours,
                       0.1f);
        }
        if (effect->duration == "periodic") {
            floatField(session, effectId, "period (s)",
                       core::fnv1a("period"), effect->period, 0.05f);
        }
    } else {
        ImGui::TextDisabled("instant: bakes into BaseValue, no fields");
    }

    ImGui::SeparatorText("Tags");
    textField(session, effectId, "granted", core::fnv1a("grantedTag"),
              effect->grantedTag);
    textField(session, effectId, "required", core::fnv1a("requiredTag"),
              effect->requiredTag);
    textField(session, effectId, "blocked", core::fnv1a("blockedTag"),
              effect->blockedTag);

    const bool resonance = effect->attribute == "onyx" ||
                           effect->attribute == "amber" ||
                           effect->attribute == "garnet";
    if (resonance) {
        ImGui::SeparatorText("Expiry (resonance)");
        keywordCombo("mode", "expiryMode", effect->expiryMode);
        if (effect->expiryMode == "decay") {
            floatField(session, effectId, "decay/game-hour",
                       core::fnv1a("decayPerHour"), effect->decayPerHour,
                       0.1f);
            floatField(session, effectId, "expiry magnitude",
                       core::fnv1a("expiryMagnitude"),
                       effect->expiryMagnitude, 0.1f);
        }
    }

    ImGui::SeparatorText("Buildup");
    keywordCombo("type", "buildupType", effect->buildupType);
    bool bypass = effect->bypassEnergyRegenDelay;
    if (ImGui::Checkbox("bypass energy regen delay", &bypass)) {
        session.setField(effectId, core::fnv1a("bypassEnergyRegenDelay"),
                         reflect::Value { bypass });
    }

    // ---- Test apply: the REAL pipeline on a throwaway actor ----------
    ImGui::SeparatorText("Test apply (throwaway actor)");
    if (ImGui::Button("Reset actor")) {
        testActor.reset();
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply effect")) {
        if (testActor.system.current.empty()) {
            testActor.reset();
        }
        for (const str& tag : { effect->grantedTag, effect->requiredTag,
                                effect->blockedTag }) {
            if (!tag.empty()) {
                testActor.tags.registerTag(tag);
            }
        }
        const bool applied = gameplay::applyEffect(testActor.set, testActor.system,
                                                   *effect, testActor.tags);
        testActor.lastResult = applied ? "applied" : "REFUSED (required/blocked tags)";
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
