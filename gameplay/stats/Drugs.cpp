#include "gameplay/stats/Drugs.hpp"

#include "gameplay/ability/Tags.hpp"

namespace gameplay {

bool harmonyBroken(const AbilitySystem& system, const GameplayTagRegistry& tags) {
    const auto tag = tags.find(tags::HarmonyBroken);
    return tag && system.tags.has(*tag);
}

} // namespace gameplay
