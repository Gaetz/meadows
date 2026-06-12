#include "data/forms/CoreForms.hpp"

#include "data/forms/FormTypeRegistry.hpp"

namespace data {

void registerCoreFormTypes(FormTypeRegistry& registry) {
    registry.registerFormType<WeaponForm>();
    registry.registerFormType<ActorForm>();
}

} // namespace data
