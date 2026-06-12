#pragma once

#include <unordered_map>

#include "data/forms/Form.hpp"
#include "engine/reflect/Reflect.hpp"

namespace data {

// The resolved view of the world's definitions: output of the §5 plugin
// resolution, read-only for gameplay. Owns the forms.
class FormDatabase {
public:
    // Takes ownership. Returns an invalid handle (and logs) if the guid is
    // missing or already present.
    FormHandle add(uptr<Form> form, const reflect::TypeInfo& type);

    const Form* find(const core::Guid& id) const;
    const Form* get(FormHandle handle) const;
    FormHandle handleOf(const core::Guid& id) const;
    const reflect::TypeInfo* typeOf(FormHandle handle) const;

    // Typed lookup; nullptr if absent or not a T (checked via isA).
    template<typename T>
    const T* find(const core::Guid& id) const {
        const FormHandle handle = handleOf(id);
        if (!handle.isValid()) {
            return nullptr;
        }
        if (!typeOf(handle)->isA(T::staticTypeInfo().id)) {
            return nullptr;
        }
        return static_cast<const T*>(get(handle));
    }

    u32 count() const { return static_cast<u32>(entries.size()); }

private:
    struct Entry {
        uptr<Form> form;
        const reflect::TypeInfo* type { nullptr };
    };

    vector<Entry> entries; // FormHandle::value = index + 1
    std::unordered_map<core::Guid, u32> indexByGuid;
};

} // namespace data
