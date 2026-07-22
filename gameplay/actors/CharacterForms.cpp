#include "gameplay/actors/CharacterForms.hpp"

#include "data/forms/FormQuery.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "engine/core/Rng.hpp"
#include "gameplay/inventory/Inventory.hpp"

namespace gameplay {

void registerCharacterFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<AppearanceForm>();
    registry.registerFormType<ActorTagForm>();
    registry.registerFormType<LoadoutEntryForm>();
}

void applyLoadout(const data::FormDatabase& forms, const core::Guid& actor,
                  Inventory& inventory, core::Rng& rng) {
    data::childrenOf<LoadoutEntryForm>(
        forms, actor, [&](const LoadoutEntryForm& entry) {
            if (entry.count <= 0 || !entry.item.isValid()) {
                return;
            }
            if (entry.chance < 1.0f && !rng.chance(entry.chance)) {
                return;
            }
            addItem(inventory, entry.item, entry.count);
        });
}

} // namespace gameplay
