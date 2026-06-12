#pragma once

#include <string_view>
#include <unordered_map>

#include "engine/reflect/Reflect.hpp"

namespace reflect {

// Explicit type registry: each module exposes a registerXxxTypes(Registry&)
// function called at startup — no self-registering statics, so registration
// order is deterministic and visible (§8: no hidden global state).
class Registry {
public:
    template<typename T>
    void registerType() {
        add(T::staticTypeInfo());
    }

    const TypeInfo* find(u32 typeId) const;
    const TypeInfo* find(std::string_view typeName) const;

private:
    void add(const TypeInfo& info);

    std::unordered_map<u32, const TypeInfo*> types;
};

} // namespace reflect
