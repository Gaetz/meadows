#include "engine/reflect/Registry.hpp"

#include "engine/core/Assert.hpp"
#include "engine/core/Log.hpp"

namespace reflect {

void Registry::add(const TypeInfo& info) {
    const auto [it, inserted] = types.emplace(info.id, &info);
    if (!inserted && it->second != &info) {
        // A colliding fnv1a(type name) or a double registration silently
        // DROPS the second type: every record of that type would then skip
        // as "unknown". A programming error, not a data error — stop the
        // debug build here instead of corrupting downstream (audit U1-02).
        LOG_ERROR("Reflection: type id collision or double registration "
                  "for '{}' vs '{}'",
                  info.name, it->second->name);
        ENGINE_ASSERT_MSG(false,
                          "reflection type id collision: '{}' vs '{}'",
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
