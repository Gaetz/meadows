#include "data/forms/FormDatabase.hpp"

#include "engine/core/Log.hpp"

namespace data {

FormHandle FormDatabase::add(uptr<Form> form, const reflect::TypeInfo& type) {
    if (!form || !form->id.isValid()) {
        LOG_ERROR("FormDatabase: rejected form with invalid guid");
        return {};
    }
    if (indexByGuid.contains(form->id)) {
        LOG_ERROR("FormDatabase: duplicate form guid {}", form->id.toString());
        return {};
    }

    const u32 index = static_cast<u32>(entries.size());
    indexByGuid.emplace(form->id, index);
    entries.push_back({ std::move(form), &type });
    const FormHandle handle { index + 1 };
    indexForm(handle, *entries.back().form, type);
    return handle;
}

void FormDatabase::indexForm(FormHandle handle, const Form& form,
                             const reflect::TypeInfo& type) {
    for (const reflect::TypeInfo* walk = &type; walk;
         walk = walk->parent) {
        byType[walk->id].push_back(handle);
    }
    static const u32 kParentField = core::fnv1a("parent");
    if (const reflect::FieldInfo* field = type.findField(kParentField);
        field && field->kind == reflect::FieldKind::Guid) {
        const auto parent = std::get<core::Guid>(field->get(&form));
        if (parent.isValid()) {
            byParent[parent].push_back(handle);
        }
    }
}

const vector<FormHandle>& FormDatabase::handlesByType(u32 typeId) const {
    static const vector<FormHandle> kNone;
    const auto it = byType.find(typeId);
    return it != byType.end() ? it->second : kNone;
}

const vector<FormHandle>& FormDatabase::childHandles(
    const core::Guid& parent) const {
    static const vector<FormHandle> kNone;
    const auto it = byParent.find(parent);
    return it != byParent.end() ? it->second : kNone;
}

void FormDatabase::rebuildIndexes() {
    byType.clear();
    byParent.clear();
    for (u32 i = 0; i < entries.size(); ++i) {
        indexForm(FormHandle { i + 1 }, *entries[i].form, *entries[i].type);
    }
}

const Form* FormDatabase::find(const core::Guid& id) const {
    return get(handleOf(id));
}

const Form* FormDatabase::get(FormHandle handle) const {
    if (!handle.isValid() || handle.value > entries.size()) {
        return nullptr;
    }
    return entries[handle.value - 1].form.get();
}

FormHandle FormDatabase::handleOf(const core::Guid& id) const {
    const auto it = indexByGuid.find(id);
    return it != indexByGuid.end() ? FormHandle { it->second + 1 }
                                   : FormHandle {};
}

const reflect::TypeInfo* FormDatabase::typeOf(FormHandle handle) const {
    if (!handle.isValid() || handle.value > entries.size()) {
        return nullptr;
    }
    return entries[handle.value - 1].type;
}

} // namespace data
