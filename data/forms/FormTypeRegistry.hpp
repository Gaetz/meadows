#pragma once

#include <unordered_map>

#include "data/forms/Form.hpp"
#include "engine/reflect/Registry.hpp"

namespace data {

// Reflection registry + factory for form types: everything the plugin
// loader and resolver need to turn a record's type name into a default-
// constructed instance with patchable fields.
class FormTypeRegistry {
public:
    template<typename T>
    void registerFormType() {
        static_assert(std::is_base_of_v<Form, T>);
        reflectRegistry.registerType<T>();
        factories.emplace(T::staticTypeInfo().id,
                          []() -> uptr<Form> { return std::make_unique<T>(); });
    }

    const reflect::TypeInfo* findType(u32 typeId) const {
        return reflectRegistry.find(typeId);
    }
    const reflect::TypeInfo* findType(std::string_view typeName) const {
        return reflectRegistry.find(typeName);
    }

    // Default-constructed instance (C++ defaults = layer zero of the §5
    // resolution); nullptr for an unknown type id.
    uptr<Form> instantiate(u32 typeId) const {
        const auto it = factories.find(typeId);
        return it != factories.end() ? it->second() : nullptr;
    }

    // Visits every registered form type (editor type lists, console).
    // Unordered — callers sort by name for display.
    template<typename Fn>
    void forEachType(Fn&& fn) const {
        for (const auto& [typeId, factory] : factories) {
            if (const reflect::TypeInfo* type = findType(typeId)) {
                fn(*type);
            }
        }
    }

private:
    reflect::Registry reflectRegistry;
    std::unordered_map<u32, uptr<Form> (*)()> factories;
};

} // namespace data
