#include "gameplay/stats/Skills.hpp"

#include <algorithm>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/event/EventBus.hpp"

namespace gameplay {

void registerSkillFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<SkillForm>();
    registry.registerFormType<SkillThresholdForm>();
}

void awardSkillUse(const data::FormDatabase& forms, const core::Guid& skill,
                   SkillProgress& progress, AttributeSet& attributes,
                   AbilitySystem& system, const GameplayTagRegistry& tags) {
    const SkillForm* form = forms.find<SkillForm>(skill);
    if (!form) {
        return; // skill from an unloaded mod: never fatal (§5)
    }
    SkillEntry& entry = progress.skills[skill];
    entry.xp += form->xpPerUse;

    // Ranks in ascending-xp order (ties broken by guid — deterministic
    // whatever the load order); `granted` counts the applied prefix.
    vector<const SkillThresholdForm*> thresholds =
        data::collectChildren<SkillThresholdForm>(forms, skill);
    std::sort(thresholds.begin(), thresholds.end(),
              [](const SkillThresholdForm* a, const SkillThresholdForm* b) {
                  return a->xp != b->xp ? a->xp < b->xp : a->id < b->id;
              });
    for (size_t i = static_cast<size_t>(entry.granted);
         i < thresholds.size() && thresholds[i]->xp <= entry.xp; ++i) {
        if (const EffectForm* effect =
                forms.find<EffectForm>(thresholds[i]->effect)) {
            applyEffect(attributes, system, *effect, tags);
        }
        entry.granted = static_cast<i32>(i) + 1;
    }
}

u32 bindSkillProgression(EventBus& bus, const data::FormDatabase& forms,
                         const GameplayTagRegistry& tags) {
    return bus.subscribe(
        eventKind("OnAbilityUsed"),
        [&forms, &tags](const Event& event) {
            ecs::Entity entity = event.source;
            if (!entity.is_alive() || !entity.has<SkillProgress>() ||
                !entity.has<AttributeSet>() ||
                !entity.has<AbilitySystem>()) {
                return;
            }
            const AbilityForm* ability =
                data::findByEditorId<AbilityForm>(forms, event.name);
            if (!ability || !ability->skill.isValid()) {
                return;
            }
            awardSkillUse(forms, ability->skill,
                          entity.get_mut<SkillProgress>(),
                          entity.get_mut<AttributeSet>(),
                          entity.get_mut<AbilitySystem>(), tags);
        });
}

} // namespace gameplay
