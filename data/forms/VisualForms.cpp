#include "data/forms/VisualForms.hpp"

#include "data/forms/FormTypeRegistry.hpp"

namespace data {

void registerVisualFormTypes(FormTypeRegistry& registry) {
    registry.registerFormType<MaterialForm>();
    registry.registerFormType<StaticForm>();
    registry.registerFormType<LightForm>();
    registry.registerFormType<WaterVolumeForm>();
    registry.registerFormType<ParticleForm>();
    registry.registerFormType<CueForm>();
}

} // namespace data
