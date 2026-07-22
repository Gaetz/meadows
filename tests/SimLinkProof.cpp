// The §2.10 LINK-LEVEL lock. This binary links the ENTIRE sim —
// the data/ecs/gameplay/world/script/narrative archives, whole-archive — and
// deliberately NOT meadows-render. If any object in those libs references a
// render/rhi/GL symbol, this target fails to link, which is the proof the
// headless suite could not give (headers were clean, the archives were not
// checked). main() boots a token slice so the exe also proves the sim RUNS
// renderer-free, not just links.
//
// Assumed residue (documented, §2.10 strict is a later step): SDL/platform
// still live in the base `meadows` lib, so this proves render-free, not
// platform-free.

#include "data/forms/CoreForms.hpp"
#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "engine/ecs/World.hpp"
#include "gameplay/ability/AbilitySystem.hpp"

int main() {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    data::FormDatabase forms;
    ecs::World world;
    gameplay::registerGameplayComponents(world);
    ecs::Entity actor = world.create();
    actor.set<gameplay::AttributeSet>({});
    return actor.is_alive() ? 0 : 1;
}
