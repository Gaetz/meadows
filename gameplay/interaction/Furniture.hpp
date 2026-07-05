#pragma once

#include <optional>
#include <unordered_map>

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"

// Furniture occupancy (horizontal pass H7): who is using which use point
// of which placed furniture. Shared player/NPC by design — a user is an
// opaque u64 (entity id). Pure bookkeeping, headless, doctested.
//
// HOW TO FILL (post-7/07, "vivant" vertical): the furniture-use flow is
//   claim -> walk to the point (nav) -> play enter anim (animTag gate)
//   -> apply FurnitureForm.effect (GAS) while seated -> release on exit;
// NPCs queue when full (the AI package retries or wanders nearby);
// the player path also opens FurnitureForm.screen (crafting).

namespace gameplay {

class FurnitureOccupancy {
public:
    // Claims the first free point [0, pointCount) of the furniture
    // REFERENCE `furnitureRef`. One claim per user (re-claiming moves).
    std::optional<u32> claim(const core::Guid& furnitureRef, u32 pointCount,
                             u64 user);
    void release(u64 user);

    bool isFree(const core::Guid& furnitureRef, u32 point) const;
    std::optional<u32> pointOf(u64 user) const;
    u32 occupantCount(const core::Guid& furnitureRef) const;

private:
    struct Claim {
        core::Guid furniture;
        u32 point { 0 };
    };
    std::unordered_map<u64, Claim> claims; // by user
};

} // namespace gameplay
