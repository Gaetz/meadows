#include "gameplay/ability/GameplayTags.hpp"

#include "engine/core/Hash.hpp"

namespace gameplay {

GameplayTag GameplayTagRegistry::registerTag(std::string_view name) {
    if (name.empty()) {
        return {};
    }
    const GameplayTag tag { core::fnv1a(name) };
    if (tags.contains(tag.id)) {
        return tag; // already registered (with its ancestors)
    }

    GameplayTag parent {};
    if (const auto dot = name.rfind('.'); dot != std::string_view::npos) {
        parent = registerTag(name.substr(0, dot));
    }
    tags.emplace(tag.id, Entry { str { name }, parent });
    return tag;
}

std::optional<GameplayTag> GameplayTagRegistry::find(
    std::string_view name) const {
    const GameplayTag tag { core::fnv1a(name) };
    return tags.contains(tag.id) ? std::optional { tag } : std::nullopt;
}

const str* GameplayTagRegistry::nameOf(GameplayTag tag) const {
    const auto it = tags.find(tag.id);
    return it != tags.end() ? &it->second.name : nullptr;
}

GameplayTag GameplayTagRegistry::parentOf(GameplayTag tag) const {
    const auto it = tags.find(tag.id);
    return it != tags.end() ? it->second.parent : GameplayTag {};
}

bool GameplayTagRegistry::isA(GameplayTag tag, GameplayTag ancestor) const {
    if (!ancestor.isValid()) {
        return false;
    }
    for (GameplayTag t = tag; t.isValid(); t = parentOf(t)) {
        if (t == ancestor) {
            return true;
        }
    }
    return false;
}

void TagContainer::add(GameplayTag tag, const GameplayTagRegistry& registry) {
    for (GameplayTag t = tag; t.isValid(); t = registry.parentOf(t)) {
        ++counts[t.id];
    }
}

void TagContainer::remove(GameplayTag tag, const GameplayTagRegistry& registry) {
    for (GameplayTag t = tag; t.isValid(); t = registry.parentOf(t)) {
        const auto it = counts.find(t.id);
        if (it != counts.end() && --it->second <= 0) {
            counts.erase(it);
        }
    }
}

bool TagContainer::has(GameplayTag tag) const {
    const auto it = counts.find(tag.id);
    return it != counts.end() && it->second > 0;
}

} // namespace gameplay
