#pragma once

#include <unordered_map>

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"

namespace data {
class FormTypeRegistry;
}

namespace world {

// The fixed set of spawnable base-form categories (§2.7). The spawner keys on a
// base form's category to wire the right mandatory components. New form *types*
// are not added by mods; richness comes from data on these categories, effects
// and scripts.
enum class FormCategory : u8 {
    Static,
    Item,
    Actor,
    Container,
    Door,
};

// Maps a base form's reflected type id to its category. Explicit (no hidden
// statics), populated once at startup, mirroring the FormTypeRegistry pattern.
class FormCategoryRegistry {
public:
    void set(u32 typeId, FormCategory category) {
        categories.insert_or_assign(typeId, category);
    }

    template<typename T>
    void set(FormCategory category) {
        set(T::staticTypeInfo().id, category);
    }

    std::optional<FormCategory> categoryOf(u32 typeId) const {
        const auto it = categories.find(typeId);
        return it != categories.end() ? std::optional { it->second }
                                      : std::nullopt;
    }

private:
    std::unordered_map<u32, FormCategory> categories;
};

// Registers categories for the core base form types (WeaponForm, ActorForm...).
void registerCoreCategories(FormCategoryRegistry& registry);

} // namespace world
