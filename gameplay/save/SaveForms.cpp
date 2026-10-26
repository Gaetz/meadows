#include "gameplay/save/SaveForms.hpp"

#include <utility>

#include "data/forms/FormTypeRegistry.hpp"
#include "engine/core/Assert.hpp"
#include "gameplay/save/SaveState.hpp"

namespace gameplay {

namespace {

// The generated half of SavedStatsForm: its TypeInfo and the per-slot
// defaults, built together so a slot's kind and its default never drift
// apart. One field per distinct non-excluded component field name
// (parents of the component types included), in sweep order; a name met
// again shares the earlier slot and must agree on the kind.
struct GeneratedStats {
    reflect::TypeInfo info;
    vector<reflect::Value> defaults;
};

const GeneratedStats& generatedStats() {
    static const GeneratedStats kGenerated = [] {
        GeneratedStats out;
        out.info = reflect::TypeInfoBuilder<SavedStatsForm> {
            "SavedStatsForm", &data::Form::staticTypeInfo() }
                       .field("parent", &SavedStatsForm::parent)
                       .build();
        const auto ownField = [&out](u32 id) -> const reflect::FieldInfo* {
            for (const reflect::FieldInfo& field : out.info.fields) {
                if (field.id == id) {
                    return &field;
                }
            }
            return nullptr;
        };
        for (const SavedStatsComponent& component : savedStatsComponents()) {
            reflect::forEachField(*component.type, [&](const reflect::FieldInfo& field) {
                if (isSavedStatsExcluded(component.type->name, field.name)) {
                    return;
                }
                if (const reflect::FieldInfo* shared = ownField(field.id)) {
                    ENGINE_ASSERT_MSG(shared->kind == field.kind,
                                      "SavedStatsForm.{}: {} reflects a "
                                      "different kind than the component "
                                      "generated before it",
                                      field.name, component.type->name);
                    return;
                }
                // A component field named after an inherited Form field
                // (editorId) would alias record identity: refuse it.
                ENGINE_ASSERT_MSG(
                    data::Form::staticTypeInfo().findField(field.id) == nullptr,
                    "SavedStatsForm.{}: {} reflects a name data::Form owns",
                    field.name, component.type->name);
                const size_t slot = out.defaults.size();
                out.defaults.push_back(field.get(component.defaults));
                reflect::FieldInfo generated;
                generated.name = field.name;
                generated.id = field.id;
                generated.kind = field.kind;
                generated.get = [slot](const void* object) -> reflect::Value {
                    return static_cast<const SavedStatsForm*>(object)
                        ->values[slot];
                };
                generated.set = [slot, kind = field.kind](
                                    void* object,
                                    const reflect::Value& value) -> bool {
                    if (reflect::valueKind(value) != kind) {
                        return false;
                    }
                    static_cast<SavedStatsForm*>(object)->values[slot] = value;
                    return true;
                };
                out.info.fields.push_back(std::move(generated));
            });
        }
        return out;
    }();
    return kGenerated;
}

} // namespace

SavedStatsForm::SavedStatsForm() : values(generatedStats().defaults) {}

const reflect::TypeInfo& SavedStatsForm::staticTypeInfo() {
    return generatedStats().info;
}

void registerSaveFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<SavedStatsForm>();
    registry.registerFormType<SavedEffectForm>();
    registry.registerFormType<SavedItemForm>();
    registry.registerFormType<SavedInjuryForm>();
    registry.registerFormType<SavedAbilityForm>();
    registry.registerFormType<SavedSkillForm>();
    registry.registerFormType<SavedBountyForm>();
    registry.registerFormType<SavedScriptVarForm>();
    registry.registerFormType<WorldStateForm>();
}

} // namespace gameplay
