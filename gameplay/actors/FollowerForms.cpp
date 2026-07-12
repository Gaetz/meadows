#include "gameplay/actors/FollowerForms.hpp"

#include <algorithm>

#include "data/forms/FormTypeRegistry.hpp"

namespace gameplay {

void registerFollowerFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<FollowerClassForm>(); // FOLLOWERS É0
    registry.registerFormType<ClassPerkForm>();     // FOLLOWERS É0
}

CoreAttributes classAttributesAt(const FollowerClassForm& cls, f32 level) {
    const f32 n = std::max(level, 1.0f) - 1.0f;
    CoreAttributes result;
    result.strength = cls.strengthBase + cls.strengthPerLevel * n;
    result.constitution = cls.constitutionBase + cls.constitutionPerLevel * n;
    result.grace = cls.graceBase + cls.gracePerLevel * n;
    result.dexterity = cls.dexterityBase + cls.dexterityPerLevel * n;
    result.alacrity = cls.alacrityBase + cls.alacrityPerLevel * n;
    result.perception = cls.perceptionBase + cls.perceptionPerLevel * n;
    result.charisma = cls.charismaBase + cls.charismaPerLevel * n;
    result.ego = cls.egoBase + cls.egoPerLevel * n;
    result.insight = cls.insightBase + cls.insightPerLevel * n;
    return result;
}

} // namespace gameplay
