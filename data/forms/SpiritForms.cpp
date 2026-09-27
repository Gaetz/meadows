#include "data/forms/SpiritForms.hpp"

#include "data/forms/FormTypeRegistry.hpp"

namespace data {

void registerSpiritFormTypes(FormTypeRegistry& registry) {
    registry.registerFormType<SpiritForm>();
    registry.registerFormType<SpiritRuleForm>();
    registry.registerFormType<SurfaceMaterialForm>();
    registry.registerFormType<SpellForm>();
}

} // namespace data
