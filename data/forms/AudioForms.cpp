#include "data/forms/AudioForms.hpp"

#include "data/forms/FormTypeRegistry.hpp"

namespace data {

void registerAudioFormTypes(FormTypeRegistry& registry) {
    registry.registerFormType<SoundForm>();
    registry.registerFormType<SoundVariantForm>();
}

} // namespace data
