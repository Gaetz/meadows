#include "game/ui/ConditionBuilder.hpp"

#include <algorithm>

#include <imgui.h>

#include "game/ui/FieldWidgets.hpp"
#include "game/ui/FormPicker.hpp"
#include "game/ui/Keywords.hpp"
#include "gameplay/condition/Condition.hpp"

namespace game {

void drawConditionList(data::EditSession& session, const core::Guid& parent,
                       core::Guid& selected) {
    ImGui::SeparatorText("Conditions");
    vector<std::pair<core::Guid, const gameplay::ConditionForm*>> clauses;
    session.forEachVisible([&](const core::Guid& id, const data::Form& form,
                               const reflect::TypeInfo& type) {
        if (type.id != gameplay::ConditionForm::staticTypeInfo().id) {
            return;
        }
        const auto* clause =
            static_cast<const gameplay::ConditionForm*>(&form);
        if (clause->parent == parent) {
            clauses.emplace_back(id, clause);
        }
    });
    std::sort(clauses.begin(), clauses.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [id, clause] : clauses) {
        ImGui::PushID(id.toString().c_str());
        if (session.isCreated(id)) {
            if (ImGui::SmallButton("x")) {
                session.removeCreated(id);
                ImGui::PopID();
                continue;
            }
        } else {
            ImGui::TextDisabled("o"); // base record: §5-immutable marker
        }
        ImGui::SameLine();
        if (ImGui::Selectable(
                (gameplay::conditionSummary(*clause) + "##cb").c_str(),
                selected == id)) {
            selected = id;
        }
        if (selected == id) {
            // Contextual editors: only what this kind actually reads.
            ImGui::Indent();
            const auto* kinds = keywordsFor("ConditionForm", "kind");
            str pickedKind;
            if (kinds && drawKeywordCombo("kind", *kinds, clause->kind,
                                          pickedKind)) {
                session.setField(id, core::fnv1a("kind"),
                                 reflect::Value { pickedKind });
            }
            if (clause->kind == "HasTag") {
                textField(session, id, "tag", core::fnv1a("tag"),
                          clause->tag);
            } else if (clause->kind == "AttributeAtLeast" ||
                       clause->kind == "AttributeAtMost") {
                textField(session, id, "attribute",
                          core::fnv1a("attribute"), clause->attribute);
                floatField(session, id, "value", core::fnv1a("value"),
                           clause->value);
            } else if (clause->kind == "HasItem") {
                core::Guid picked;
                if (drawItemPicker("item", session, clause->item, picked)) {
                    session.setField(id, core::fnv1a("item"),
                                     reflect::Value { picked });
                }
                floatField(session, id, "count", core::fnv1a("value"),
                           clause->value);
            } else if (clause->kind == "Lua") {
                textField(session, id, "expression", core::fnv1a("lua"),
                          clause->lua);
            }
            bool negate = clause->negate;
            if (ImGui::Checkbox("negate", &negate)) {
                session.setField(id, core::fnv1a("negate"),
                                 reflect::Value { negate });
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }

    if (ImGui::SmallButton("+ condition")) {
        const data::Form* parentForm = session.view(parent);
        const core::Guid id = session.createForm(
            gameplay::ConditionForm::staticTypeInfo().id,
            (parentForm && !parentForm->editorId.empty()
                 ? parentForm->editorId
                 : str { "New" }) +
                "Cond");
        session.setField(id, core::fnv1a("parent"),
                         reflect::Value { parent });
        selected = id;
    }
}

} // namespace game
