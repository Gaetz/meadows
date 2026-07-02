#include <doctest/doctest.h>

#include <memory>

#include "data/forms/FormDatabase.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/ability/GameplayEffects.hpp"

using core::Guid;
using namespace gameplay;

namespace {
const Guid kCost = *Guid::fromString("d0000000-0000-4000-8000-000000000001");
const Guid kAbility = *Guid::fromString("da000000-0000-4000-8000-000000000001");
} // namespace

TEST_CASE("exhaustion: State.Exhausted toggles from energy with hysteresis") {
    GameplayTagRegistry reg;
    reg.registerTag("State.Exhausted");

    AttributeSet set; // maxEnergy defaults to 100
    AbilitySystem sys;
    setBaseValue(set, attr("energy"), 0.0f);
    initializeCurrent(sys, set);

    const auto exhausted = *reg.find("State.Exhausted");

    // Energy at zero → gate engages.
    updateExhaustion(set, sys, reg);
    CHECK(sys.tags.has(exhausted));

    // Partial recovery below the 20% threshold keeps it engaged (hysteresis).
    set.energy = 10.0f;
    updateExhaustion(set, sys, reg);
    CHECK(sys.tags.has(exhausted));

    // Recovery above the threshold clears it.
    set.energy = 25.0f;
    updateExhaustion(set, sys, reg);
    CHECK_FALSE(sys.tags.has(exhausted));
}

TEST_CASE("exhaustion: gate blocks an energy-costed ability via blockedTag") {
    data::FormDatabase db;
    GameplayTagRegistry reg;
    reg.registerTag("State.Exhausted");

    // A dodge-like ability: costs energy, blocked while Exhausted.
    auto cost = std::make_unique<EffectForm>();
    cost->id = kCost;
    cost->attribute = "energy";
    cost->op = "add";
    cost->magnitude = -15.0f;
    cost->duration = "instant";
    db.add(std::move(cost), EffectForm::staticTypeInfo());

    auto ability = std::make_unique<AbilityForm>();
    ability->id = kAbility;
    ability->cost = kCost;
    ability->blockedTag = "State.Exhausted";
    db.add(std::move(ability), AbilityForm::staticTypeInfo());

    AttributeSet set; // full energy
    AbilitySystem sys;
    initializeCurrent(sys, set);

    const AbilityForm& dodge = *db.find<AbilityForm>(kAbility);
    const AbilityContext ctx { db, reg };

    // With energy and no gate, the ability activates.
    CHECK(tryActivate(dodge, set, sys, set, sys, ctx));

    // Drain to zero and run the gate: State.Exhausted engages, activation blocked.
    setBaseValue(set, attr("energy"), 0.0f);
    initializeCurrent(sys, set);
    updateExhaustion(set, sys, reg);
    CHECK(sys.tags.has(*reg.find("State.Exhausted")));
    CHECK_FALSE(tryActivate(dodge, set, sys, set, sys, ctx));
}

TEST_CASE("energy regen delay: spending energy pauses regen; bypass opts out") {
    GameplayTagRegistry reg;
    AttributeSet set;
    AbilitySystem sys;
    initializeCurrent(sys, set);

    EffectForm cost;
    cost.attribute = "energy";
    cost.op = "add";
    cost.magnitude = -20.0f;
    cost.duration = "instant";

    // Spending energy arms the recharge delay.
    CHECK(sys.energyRegenDelay == 0.0f);
    CHECK(applyEffect(set, sys, cost, reg));
    CHECK(sys.energyRegenDelay > 0.0f);

    // An effect that opts out does not arm it.
    sys.energyRegenDelay = 0.0f;
    cost.bypassEnergyRegenDelay = true;
    CHECK(applyEffect(set, sys, cost, reg));
    CHECK(sys.energyRegenDelay == 0.0f);

    // Gaining energy (a positive change) never arms it.
    EffectForm gain;
    gain.attribute = "energy";
    gain.op = "add";
    gain.magnitude = 10.0f;
    gain.duration = "instant";
    CHECK(applyEffect(set, sys, gain, reg));
    CHECK(sys.energyRegenDelay == 0.0f);
}
