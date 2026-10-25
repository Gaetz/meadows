#include "data/plugins/Validate.hpp"

#include <unordered_set>

#include "data/forms/FormDatabase.hpp"
#include "engine/reflect/ValueText.hpp"

namespace data {

ValidationReport validatePlugins(const vector<const Plugin*>& loadOrder,
                                 const FormTypeRegistry& types,
                                 const vector<core::Guid>& runtimeProvided) {
    ValidationReport report;
    FormDatabase database;
    report.resolve = resolve(loadOrder, types, database);

    // Everything a Guid field may legitimately point at: resolved forms
    // and declared assets (the VFS layer — AnimClipForm.asset, models...).
    std::unordered_set<core::Guid> known;
    for (u32 i = 1; i <= database.count(); ++i) {
        if (const Form* form = database.get(FormHandle { i })) {
            known.insert(form->id);
        }
    }
    for (const Plugin* plugin : loadOrder) {
        for (const AssetEntry& asset : plugin->assets) {
            known.insert(asset.id);
        }
        known.insert(plugin->id); // dependency-style references
    }
    known.insert(runtimeProvided.begin(), runtimeProvided.end());

    for (const Plugin* plugin : loadOrder) {
        for (const Record& record : plugin->records) {
            const reflect::TypeInfo* type = types.findType(record.typeId);
            if (!type) {
                continue; // the resolver already reported it
            }
            for (const auto& [fieldId, value] : record.fields) {
                const reflect::FieldInfo* field = type->findField(fieldId);
                if (field && (field->flags & reflect::Deprecated) != 0) {
                    report.deprecatedWrites.push_back(
                        { record.formId, type->name, field->name });
                }
            }
        }
    }

    for (u32 i = 1; i <= database.count(); ++i) {
        const FormHandle handle { i };
        const Form* form = database.get(handle);
        const reflect::TypeInfo* type = database.typeOf(handle);
        if (!form || !type) {
            continue;
        }
        reflect::forEachField(*type, [&](const reflect::FieldInfo& field) {
            if (field.enumeration) {
                const reflect::Value value = field.get(form);
                if (!field.enumeration->acceptsValue(value)) {
                    report.vocabularyErrors.push_back(
                        { form->id, type->name, field.name,
                          reflect::valueToString(value) });
                }
            }
            if (field.kind != reflect::FieldKind::Guid) {
                return;
            }
            const auto target = std::get<core::Guid>(field.get(form));
            if (!target.isValid() || known.contains(target)) {
                return; // null = "unset", always fine
            }
            report.danglingRefs.push_back(
                { form->id, type->name, field.name, target });
        });
    }
    return report;
}

} // namespace data
