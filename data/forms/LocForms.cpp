#include "data/forms/LocForms.hpp"

#include "data/forms/FormTypeRegistry.hpp"

namespace data {

void registerLocFormTypes(FormTypeRegistry& registry) {
    registry.registerFormType<LocStringForm>();
}

} // namespace data
