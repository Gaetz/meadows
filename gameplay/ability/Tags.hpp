#pragma once

#include <string_view>

// The runtime tag vocabulary the C++ side reads and writes, named once:
// a misspelt tag is a compile error instead of a silently inert check.
// Registered by registerCharacterRuntimeTags (gameplay/stats/StatsTuning);
// data (effects, conditions, scripts) keeps writing the same strings.
namespace gameplay::tags {

inline constexpr std::string_view Dead = "State.Dead";
inline constexpr std::string_view Staggered = "State.Staggered";
inline constexpr std::string_view Paralyzed = "State.Paralyzed";
inline constexpr std::string_view CriticalWeakness = "State.CriticalWeakness";
inline constexpr std::string_view Shaken = "State.Shaken";
inline constexpr std::string_view Downed = "State.Downed";
inline constexpr std::string_view Blocking = "State.Blocking";
inline constexpr std::string_view Dodging = "State.Dodging";
inline constexpr std::string_view Sneaking = "State.Sneaking";
inline constexpr std::string_view Resting = "State.Resting";
inline constexpr std::string_view Exhausted = "State.Exhausted";
inline constexpr std::string_view Invulnerable = "State.Invulnerable";

inline constexpr std::string_view Poisoned = "Status.Poisoned";
inline constexpr std::string_view Bleeding = "Status.Bleeding";
inline constexpr std::string_view Mental = "Status.Mental";
inline constexpr std::string_view Diseased = "Status.Diseased";
inline constexpr std::string_view Cursed = "Status.Cursed";
inline constexpr std::string_view Dying = "Status.Dying";
inline constexpr std::string_view Ignited = "Status.Ignited";
inline constexpr std::string_view Glaciated = "Status.Glaciated";
inline constexpr std::string_view Electrocuted = "Status.Electrocuted";
inline constexpr std::string_view HarmonyBroken = "Status.HarmonyBroken";
inline constexpr std::string_view Burning = "Status.Burning";

inline constexpr std::string_view FollowerProtected = "Follower.Protected";

} // namespace gameplay::tags
