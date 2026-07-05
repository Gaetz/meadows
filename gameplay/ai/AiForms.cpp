#include "gameplay/ai/AiForms.hpp"

#include "data/forms/FormTypeRegistry.hpp"

namespace gameplay {

void registerAiFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<AiPackageForm>();
    registry.registerFormType<ScheduleForm>();
    registry.registerFormType<ScheduleEntryForm>();
}

} // namespace gameplay
