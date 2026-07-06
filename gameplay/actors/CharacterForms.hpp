#pragma once

#include "data/forms/Form.hpp"

// Character visual data (horizontal pass H1). The modular-appearance model:
// a character = one skeleton + interchangeable skinned meshes per FIXED
// slot (flat fields — the slot set is a C++ decision, richness is which
// meshes fill them, §2.7). Equipping armor swaps the matching slot's mesh
// (EquipmentVisuals vertical, post-7/07); AppearanceForm holds the
// UNDRESSED defaults + tints.
//
// HOW TO FILL (post-7/07): EquipmentVisuals = ArmorForm gains slot mesh
// guids (append) + a system that recomputes the visible mesh per slot from
// (appearance, equipped items); tints multiply the material.

namespace data {
class FormTypeRegistry;
}

namespace gameplay {

struct AppearanceForm : data::Form {
    core::Guid skeleton;  // glTF asset carrying the shared skeleton
    core::Guid headMesh;  // skinned mesh assets per slot (0 = none)
    core::Guid hairMesh;
    core::Guid torsoMesh;
    core::Guid legsMesh;
    core::Guid handsMesh;
    core::Guid feetMesh;
    Vec4 skinTint { 1.0f, 1.0f, 1.0f, 1.0f };
    Vec4 hairTint { 1.0f, 1.0f, 1.0f, 1.0f };

    REFLECT_BEGIN(AppearanceForm, data::Form)
        REFLECT_FIELD(skeleton)
        REFLECT_FIELD(headMesh)
        REFLECT_FIELD(hairMesh)
        REFLECT_FIELD(torsoMesh)
        REFLECT_FIELD(legsMesh)
        REFLECT_FIELD(handsMesh)
        REFLECT_FIELD(feetMesh)
        REFLECT_FIELD(skinTint)
        REFLECT_FIELD(hairTint)
    REFLECT_END()
};

// Default gameplay tags of an actor (chantier 3 — the §C.1 NarrativePro
// mapping, first slice): CHILD records — a mod adds a faction membership
// ("Faction.Bandits") with one record. v1: the runtime reads them for
// hostility; the full grant-into-GAS (with the loadout/grants records)
// is the next slice of the character-definition work.
struct ActorTagForm : data::Form {
    core::Guid parent; // ActorForm
    str tag;           // "Faction.Bandits", "State.Invulnerable"...

    REFLECT_BEGIN(ActorTagForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(tag)
    REFLECT_END()
};

void registerCharacterFormTypes(data::FormTypeRegistry& registry);

} // namespace gameplay
