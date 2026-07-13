#include "data/plugins/Validate.hpp"

#include <unordered_set>

#include "data/forms/FormDatabase.hpp"

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

    for (u32 i = 1; i <= database.count(); ++i) {
        const FormHandle handle { i };
        const Form* form = database.get(handle);
        const reflect::TypeInfo* type = database.typeOf(handle);
        if (!form || !type) {
            continue;
        }
        reflect::forEachField(*type, [&](const reflect::FieldInfo& field) {
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
