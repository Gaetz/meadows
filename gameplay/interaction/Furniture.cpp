#include "gameplay/interaction/Furniture.hpp"

namespace gameplay {

std::optional<u32> FurnitureOccupancy::claim(const core::Guid& furnitureRef,
                                             u32 pointCount, u64 user) {
    release(user); // re-claiming moves the user
    for (u32 point = 0; point < pointCount; ++point) {
        if (isFree(furnitureRef, point)) {
            claims.emplace(user, Claim { furnitureRef, point });
            return point;
        }
    }
    return std::nullopt; // full — the caller queues or walks away
}

void FurnitureOccupancy::release(u64 user) {
    claims.erase(user);
}

bool FurnitureOccupancy::isFree(const core::Guid& furnitureRef,
                                u32 point) const {
    for (const auto& [user, claim] : claims) {
        if (claim.furniture == furnitureRef && claim.point == point) {
            return false;
        }
    }
    return true;
}

std::optional<u32> FurnitureOccupancy::pointOf(u64 user) const {
    const auto it = claims.find(user);
    return it != claims.end() ? std::optional { it->second.point }
                              : std::nullopt;
}

u32 FurnitureOccupancy::occupantCount(const core::Guid& furnitureRef) const {
    u32 count = 0;
    for (const auto& [user, claim] : claims) {
        if (claim.furniture == furnitureRef) {
            ++count;
        }
    }
    return count;
}

} // namespace gameplay
