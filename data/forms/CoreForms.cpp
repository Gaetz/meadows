#include "data/forms/CoreForms.hpp"

#include "data/forms/FormTypeRegistry.hpp"

namespace data {

void registerCoreFormTypes(FormTypeRegistry& registry) {
    registry.registerFormType<WeaponForm>();
    registry.registerFormType<ArmorForm>();
    registry.registerFormType<ConsumableForm>();
    registry.registerFormType<ActorForm>();
}

} // namespace data
