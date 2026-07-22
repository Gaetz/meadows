#include "gameplay/stats/StatsTuning.hpp"

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"
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
    for (const char* tag :
         { "State.Dead", "State.Staggered", "State.Paralyzed",
           "State.Downed",       // 0 HP under protection
           "Follower.Protected", // the followerActive mirror routing
                                 // 0 HP to Downed (updateLifeState)
           "State.Exhausted", "State.Shaken", "State.CriticalWeakness",
           "State.Blocking",  // the raised guard
           "State.Dodging",   // dodge i-frames (grantedTag — a tag the
                              // registry doesn't know is granted SILENTLY
                              // as nothing)
           "State.Resting",   // furniture effects (SeatRest...)
           "State.Sneaking",  // sneak toggle (detection + step audio)
           "Cooldown.Dodge", "Cooldown.Attack" }) {
        tags.registerTag(tag);
    }
    for (const char* statusTag :
         { "Status.Poisoned", "Status.Bleeding", "Status.Mental",
           "Status.Diseased", "Status.Cursed", "Status.Dying",
           "Status.HarmonyBroken", "Status.Ignited", "Status.Glaciated",
           "Status.Electrocuted" }) {
        tags.registerTag(statusTag);
    }
    registerStatsRuntimeTags(tags);
}

StatsTuningForm resolveStatsTuning(const data::FormDatabase& forms) {
    if (const StatsTuningForm* tuning =
            forms.find<StatsTuningForm>(kStatsTuningGuid)) {
        return *tuning;
    }
    return StatsTuningForm {};
}

} // namespace gameplay
