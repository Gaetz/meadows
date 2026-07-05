#include "data/forms/AnimForms.hpp"

#include "data/forms/FormTypeRegistry.hpp"

namespace data {

void registerAnimFormTypes(FormTypeRegistry& registry) {
    registry.registerFormType<AnimClipForm>();
    registry.registerFormType<AnimEventForm>();
    registry.registerFormType<AnimGraphForm>();
    registry.registerFormType<AnimStateForm>();
    registry.registerFormType<AnimTransitionForm>();
}

} // namespace data
