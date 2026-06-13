#pragma once

#include <optional>
#include <string_view>
#include <unordered_map>

#include "engine/core/Defines.hpp"

// GameplayTags (§6): a hierarchical vocabulary (`Status.Burning`, `State.Dead`,
// `Faction.CityGuard`) used for effect requirements/immunity, ability gating,
// faction membership (§6.1), and the Phase-4 condition evaluator. These are OUR
// moddable gameplay data — NOT flecs tags (which are ECS storage). A tag is
// interned to a stable id (fnv1a of its full dotted name); the registry records
// parent chains so containers can match ancestors.

namespace gameplay {

struct GameplayTag {
    u32 id { 0 }; // fnv1a of the full dotted name; 0 = invalid

    bool isValid() const { return id != 0; }
    bool operator==(const GameplayTag&) const = default;
};

// The tag vocabulary: interns dotted names and records hierarchy. Explicit,
// like FormTypeRegistry — no hidden statics; a data manifest can feed it, and
// for now code registers tags at startup.
class GameplayTagRegistry {
public:
    // Registers a tag and all its ancestors ("A.B.C" also registers "A.B" and
    // "A"). Idempotent. Returns the leaf tag (invalid for an empty name).
    GameplayTag registerTag(std::string_view name);

    std::optional<GameplayTag> find(std::string_view name) const;
    const str* nameOf(GameplayTag tag) const;     // nullptr if unknown
    GameplayTag parentOf(GameplayTag tag) const;   // invalid if root/unknown

    // True if `tag` is `ancestor` or a descendant of it (walks the parent chain).
    bool isA(GameplayTag tag, GameplayTag ancestor) const;

    u32 count() const { return static_cast<u32>(tags.size()); }

private:
    struct Entry {
        str name;
        GameplayTag parent;
    };
    std::unordered_map<u32, Entry> tags;
};

// A ref-counted set of owned tags. Stores the ancestor-expanded closure so that
// has() is registry-free and O(1): adding "Status.Burning" also counts "Status"
// and so on, making has("Status") true. Ref-counting means a tag stays present
// while ANY source still grants it (multiple effects can grant the same tag).
class TagContainer {
public:
    void add(GameplayTag tag, const GameplayTagRegistry& registry);
    void remove(GameplayTag tag, const GameplayTagRegistry& registry);

    // Ancestor-aware: true if the container has `tag` or any descendant of it.
    bool has(GameplayTag tag) const;

    u32 distinctCount() const { return static_cast<u32>(counts.size()); }

private:
    std::unordered_map<u32 /*tag id*/, i32 /*ref count*/> counts;
};

} // namespace gameplay
