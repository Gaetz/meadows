#pragma once

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"

namespace gameplay {
struct Inventory;
}

namespace game {

// Barter transactions (chantier 4 B5) — PURE and headless-testable. Gold
// is an ordinary MiscItemForm stack (no parallel money system): a trade
// just moves an item one way and coins the other, and fails untouched
// when either side cannot pay. Prices come from the caller (goldValue ×
// the StatsTuningForm barter multipliers).

// value × mult, floored at 1 coin (nothing trades for free).
i32 barterPrice(i32 goldValue, f32 mult);

// The player buys one `item` from the vendor at `price` coins.
bool barterBuy(gameplay::Inventory& playerBag, gameplay::Inventory& vendorBag,
               const core::Guid& item, i32 price, const core::Guid& gold);

// The player sells one `item` to the vendor at `price` coins. The vendor
// must afford it (limited wealth — restocking is the P1 economy pass).
bool barterSell(gameplay::Inventory& playerBag,
                gameplay::Inventory& vendorBag, const core::Guid& item,
                i32 price, const core::Guid& gold);

} // namespace game
