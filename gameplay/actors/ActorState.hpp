#pragma once

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"

// Small per-actor runtime state components (chantier 6). Reflected so the
// save layer captures them by field name through the captureActor sweep —
// NOT scene-level maps (a scene map dies on re-enter while the actor's
// inventory persists: a free-restock / bounty-amnesty exploit).

namespace gameplay {

// Vendor restock clock (D1): the game-time hour of the last inventory
// re-roll. The barter screen re-rolls the loadout when more than the
// restock interval has passed.
struct VendorState {
    f32 lastRestockHours { 0.0f };

    REFLECT_BEGIN(VendorState, void)
        REFLECT_FIELD(lastRestockHours)
    REFLECT_END()
};

// Crime bounty (D2) — the per-entity faction state CLAUDE.md §6.1
// sanctions as a thin component. Conditions can't see components, so the
// scene mirrors bounty > 0 into the Crime.Wanted tag (syncTag pattern).
struct Bounty {
    f32 bounty { 0.0f };

    REFLECT_BEGIN(Bounty, void)
        REFLECT_FIELD(bounty)
    REFLECT_END()
};

} // namespace gameplay
