#include "gameplay/stats/StatsTuning.hpp"

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "gameplay/ability/Tags.hpp"
#include "gameplay/stats/Injuries.hpp" // registerInjuryTags
#include "gameplay/stats/Survival.hpp" // registerSurvivalTags

namespace gameplay {

namespace {
const core::Guid kStatsTuningGuid =
    *core::Guid::fromString("57a70000-0000-4000-8000-000000000001");
} // namespace

void registerStatsFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<StatsTuningForm>();
    // AfflictionForm and DrugForm are now EffectForms — no separate registration.
}

void registerStatsRuntimeTags(GameplayTagRegistry& tags) {
    registerInjuryTags(tags);
    registerSurvivalTags(tags);
}

void registerCharacterRuntimeTags(GameplayTagRegistry& tags) {
    // The vocabulary tickCharacter / the buildup / the damage path assume:
    // life state, the combat statuses, the ten buildup tags, exhaustion,
    // plus the stats runtime tags (injuries, survival). Registration is
    // idempotent, so scenes freely add their own vocabulary on top.
    for (std::string_view tag :
         { tags::Dead, tags::Staggered, tags::Paralyzed,
           tags::Downed,            // 0 HP under protection
           tags::FollowerProtected, // the followerActive mirror routing
                                    // 0 HP to Downed (updateLifeState)
           tags::Exhausted, tags::Shaken, tags::CriticalWeakness,
           tags::Blocking,  // the raised guard
           tags::Dodging,   // dodge i-frames (grantedTag — a tag the
                            // registry doesn't know is granted SILENTLY
                            // as nothing)
           tags::Resting,   // furniture effects (SeatRest...)
           tags::Sneaking,  // sneak toggle (detection + step audio)
           std::string_view { "Cooldown.Dodge" },
           std::string_view { "Cooldown.Attack" } }) {
        tags.registerTag(tag);
    }
    for (std::string_view statusTag :
         { tags::Poisoned, tags::Bleeding, tags::Mental,
           tags::Diseased, tags::Cursed, tags::Dying,
           tags::HarmonyBroken, tags::Ignited, tags::Glaciated,
           tags::Electrocuted }) {
        tags.registerTag(statusTag);
    }
    registerStatsRuntimeTags(tags);
}

StatsTuningForm resolveStatsTuning(const data::FormDatabase& forms) {
    return data::resolveSingleton<StatsTuningForm>(forms, kStatsTuningGuid);
}

} // namespace gameplay
