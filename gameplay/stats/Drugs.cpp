#include "gameplay/stats/Drugs.hpp"

namespace gameplay {

bool harmonyBroken(const AbilitySystem& system, const GameplayTagRegistry& tags) {
    const auto tag = tags.find("Status.HarmonyBroken");
    return tag && system.tags.has(*tag);
}

} // namespace gameplay
