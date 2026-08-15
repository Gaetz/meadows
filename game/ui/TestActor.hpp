#pragma once

#include "engine/core/Defines.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayTags.hpp"

namespace game {

// The editor panels' throwaway bench actor (AbilityPanel / EffectPanel
// "Try it" sections): one AttributeSet + AbilitySystem + tag registry to
// apply an ability or effect on, without touching any scene entity.
struct TestActor {
    gameplay::AttributeSet set;
    gameplay::AbilitySystem system;
    gameplay::GameplayTagRegistry tags;
    str lastResult;

    // Fresh defaults + a seeded current overlay.
    void reset();
};

// The shared base/current table over every reflected AttributeSet field.
void drawAttributeTable(const gameplay::AttributeSet& set,
                        const gameplay::AbilitySystem& system);

} // namespace game
