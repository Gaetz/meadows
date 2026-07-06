#include "game/Barter.hpp"

#include <cmath>

#include "gameplay/inventory/Inventory.hpp"

namespace game {

i32 barterPrice(i32 goldValue, f32 mult) {
    const i32 price =
        static_cast<i32>(std::lround(static_cast<f32>(goldValue) * mult));
    return price > 1 ? price : 1;
}

bool barterBuy(gameplay::Inventory& playerBag,
               gameplay::Inventory& vendorBag, const core::Guid& item,
               i32 price, const core::Guid& gold) {
    if (item == gold || gameplay::itemCount(playerBag, gold) < price ||
        gameplay::itemCount(vendorBag, item) < 1) {
        return false;
    }
    gameplay::removeItem(vendorBag, item, 1);
    gameplay::addItem(playerBag, item, 1);
    gameplay::removeItem(playerBag, gold, price);
    gameplay::addItem(vendorBag, gold, price);
    return true;
}

bool barterSell(gameplay::Inventory& playerBag,
                gameplay::Inventory& vendorBag, const core::Guid& item,
                i32 price, const core::Guid& gold) {
    if (item == gold || gameplay::itemCount(vendorBag, gold) < price ||
        gameplay::itemCount(playerBag, item) < 1) {
        return false;
    }
    gameplay::removeItem(playerBag, item, 1);
    gameplay::addItem(vendorBag, item, 1);
    gameplay::removeItem(vendorBag, gold, price);
    gameplay::addItem(playerBag, gold, price);
    return true;
}

} // namespace game
