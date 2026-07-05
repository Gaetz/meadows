#include "data/forms/UiForms.hpp"

#include "data/forms/FormTypeRegistry.hpp"

namespace data {

void registerUiFormTypes(FormTypeRegistry& registry) {
    registry.registerFormType<UiScreenForm>();
}

} // namespace data
