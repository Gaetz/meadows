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
    return { index + 1 };
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
