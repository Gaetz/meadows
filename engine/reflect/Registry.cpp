#include "engine/reflect/Registry.hpp"

#include "engine/core/Log.hpp"

namespace reflect {

void Registry::add(const TypeInfo& info) {
    const auto [it, inserted] = types.emplace(info.id, &info);
    if (!inserted && it->second != &info) {
        LOG_ERROR("Reflection: type id collision or double registration "
                  "for '{}' vs '{}'",
                  info.name, it->second->name);
    }
}

const TypeInfo* Registry::find(u32 typeId) const {
    const auto it = types.find(typeId);
    return it != types.end() ? it->second : nullptr;
}

const TypeInfo* Registry::find(std::string_view typeName) const {
    return find(core::fnv1a(typeName));
}

} // namespace reflect
