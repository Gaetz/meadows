#include "gameplay/ability/GameplayEffects.hpp"

#include <algorithm>

#include "gameplay/stats/StatusBuildup.hpp" // for buildupType routing

namespace gameplay {

namespace {

ModifierOp parseOp(const str& op) {
    if (op == "multiply") {
        return ModifierOp::Multiply;
    }
    if (op == "override") {
        return ModifierOp::Override;
    }
    return ModifierOp::Add;
}

enum class DurationPolicy { Instant, Duration, Infinite, Periodic };

DurationPolicy parseDuration(const str& duration) {
    if (duration == "duration") {
        return DurationPolicy::Duration;
    }
    if (duration == "infinite") {
        return DurationPolicy::Infinite;
    }
    if (duration == "periodic") {
        return DurationPolicy::Periodic;
    }
    return DurationPolicy::Instant;
}

void applyModifierToBase(AttributeSet& set, u32 attrId, ModifierOp op, f32 mag) {
    const auto base = baseValueOf(set, attrId);
    if (!base) {
        return;
    }
    f32 value = *base;
    switch (op) {
    case ModifierOp::Add:      value += mag; break;
    case ModifierOp::Multiply: value *= mag; break;
    case ModifierOp::Override: value = mag; break;
    }
    setBaseValue(set, attrId, value);
}

// PostExecute (§6 meta-attribute): a write to the transient `damage` attribute
// is routed into `health` (so the damage formula stays data-driven), then reset.
void routeDamageMeta(AttributeSet& set) {
    const auto damage = baseValueOf(set, attr("damage"));
    if (!damage || *damage == 0.0f) {
        return;
    }
    if (const auto health = baseValueOf(set, attr("health"))) {
        setBaseValue(set, attr("health"), *health - *damage);
    }
    setBaseValue(set, attr("damage"), 0.0f);
}

void clampBasePair(AttributeSet& set, u32 valueId, u32 maxId) {
    const auto value = baseValueOf(set, valueId);
    const auto max = baseValueOf(set, maxId);
    if (value && max) {
        setBaseValue(set, valueId, std::clamp(*value, 0.0f, *max));
    }
}

void clampBaseVitals(AttributeSet& set) {
    clampBasePair(set, attr("health"), attr("maxHealth"));
    clampBasePair(set, attr("energy"), attr("maxEnergy"));
    clampBasePair(set, attr("essence"), attr("maxEssence"));
}

// Aggregates the non-periodic effect modifiers (plus any continuous `extra`
// modifiers) targeting `id` onto `base`: (base + Σadd)·Πmult, with Override
// winning. The shared core of recompute.
f32 aggregateModifiers(const AbilitySystem& system, u32 id, f32 base,
                       const StatModifiers* extra) {
    f32 addSum = 0.0f;
    f32 mulProduct = 1.0f;
    bool hasOverride = false;
    f32 overrideValue = 0.0f;
    for (const ActiveEffect& active : system.activeEffects) {
        if (active.period > 0.0f || active.attribute != id) {
            continue; // periodic effects act on BaseValue, not aggregation
        }
        switch (active.op) {
        case ModifierOp::Add:      addSum += active.magnitude; break;
        case ModifierOp::Multiply: mulProduct *= active.magnitude; break;
        case ModifierOp::Override: hasOverride = true;
                                   overrideValue = active.magnitude; break;
        }
    }
    if (extra) {
        if (const auto it = extra->add.find(id); it != extra->add.end()) {
            addSum += it->second;
        }
        if (const auto it = extra->mul.find(id); it != extra->mul.end()) {
            mulProduct *= it->second;
        }
    }
    const f32 current = (base + addSum) * mulProduct;
    return hasOverride ? overrideValue : current;
}

bool setPresent(std::span<const AttrSetRef> sets, const reflect::TypeInfo* type) {
    for (const AttrSetRef& ref : sets) {
        if (ref.type == type) {
            return true;
        }
    }
    return false;
}

f32 readF32(const reflect::FieldInfo& field, const void* instance) {
    const reflect::Value value = field.get(instance);
    const f32* result = std::get_if<f32>(&value);
    return result ? *result : 0.0f;
}

void clampVitalsCurrent(AbilitySystem& system) {
    const auto clampCurrent = [&](u32 valueId, u32 maxId) {
        const auto value = system.current.find(valueId);
        const auto max = system.current.find(maxId);
        if (value != system.current.end() && max != system.current.end()) {
            value->second = std::clamp(value->second, 0.0f, max->second);
        }
    };
    clampCurrent(attr("health"), attr("maxHealth"));
    clampCurrent(attr("energy"), attr("maxEnergy"));
    clampCurrent(attr("essence"), attr("maxEssence"));
}

// Removes the granted tag for an expired effect (if any) and erases it.
void expireEffect(AbilitySystem& system, const ActiveEffect& active,
                  const GameplayTagRegistry& registry) {
    if (active.grantedTag.isValid()) {
        system.tags.remove(active.grantedTag, registry);
    }
}

} // namespace

void recomputeCurrent(AbilitySystem& system, std::span<const AttrSetRef> sets,
                      const DerivedStatRegistry* derived,
                      const StatModifiers* extra) {
    // Which derived calculators apply this recompute (their source set present)?
    // Pass 1 skips their targets; pass 2 fills them from their formula.
    vector<const DerivedStat*> applied;
    if (derived) {
        for (const DerivedStat& stat : derived->all()) {
            if (stat.formula &&
                (stat.sourceSet == nullptr || setPresent(sets, stat.sourceSet))) {
                applied.push_back(&stat);
            }
        }
    }
    const auto isDerivedTarget = [&](u32 id) {
        for (const DerivedStat* stat : applied) {
            if (stat->target == id) {
                return true;
            }
        }
        return false;
    };

    // Pass 1 — non-derived fields across every set: aggregate over the base.
    for (const AttrSetRef& ref : sets) {
        if (!ref.type || !ref.instance) {
            continue;
        }
        for (const reflect::FieldInfo& field : ref.type->fields) {
            if (field.kind != reflect::FieldKind::F32 || isDerivedTarget(field.id)) {
                continue;
            }
            system.current[field.id] = aggregateModifiers(
                system, field.id, readF32(field, ref.instance), extra);
        }
    }

    // Pass 2 — derived fields: aggregate over the formula. Sources (the nine
    // attributes) are non-derived, already computed in pass 1.
    const StatView view { system, sets };
    for (const DerivedStat* stat : applied) {
        system.current[stat->target] =
            aggregateModifiers(system, stat->target, stat->formula(view), extra);
    }

    clampVitalsCurrent(system);
}

void recomputeCurrent(const AttributeSet& set, AbilitySystem& system) {
    const AttrSetRef one { &AttributeSet::staticTypeInfo(), &set };
    recomputeCurrent(system, std::span<const AttrSetRef> { &one, 1 }, nullptr);
}

bool applyEffect(AttributeSet& set, AbilitySystem& system,
                 const EffectForm& effect, const GameplayTagRegistry& registry,
                 StatusBuildup* buildup, u32* outEffectId) {
    // Tag gate.
    if (!effect.requiredTag.empty()) {
        const auto tag = registry.find(effect.requiredTag);
        if (!tag || !system.tags.has(*tag)) {
            return false;
        }
    }
    if (!effect.blockedTag.empty()) {
        const auto tag = registry.find(effect.blockedTag);
        if (tag && system.tags.has(*tag)) {
            return false; // immunity
        }
    }

    // StatusBuildup routing: if buildupType is set, delegate and return.
    if (!effect.buildupType.empty() && buildup) {
        const StatusType st = parseStatusType(effect.buildupType);
        tryAddBuildup(*buildup, st, effect.magnitude, system, registry);
        return true;
    }

    const u32 attrId = attr(effect.attribute);
    const ModifierOp op = parseOp(effect.op);
    const DurationPolicy policy = parseDuration(effect.duration);

    // Spending energy pauses its regen for a beat (unless the effect opts out via
    // bypassEnergyRegenDelay), so rapid energy use carries a recharge cost.
    // CharacterTick counts the timer down and holds regen while it is > 0.
    // Duration is a constant for now (→ StatsTuningForm later, like the exhaustion
    // gate). Refreshes (max) so back-to-back spends don't shorten the pause.
    if (!effect.bypassEnergyRegenDelay && op == ModifierOp::Add &&
        effect.magnitude < 0.0f && attrId == attr("energy")) {
        constexpr f32 kEnergyRegenDelay = 1.0f;
        system.energyRegenDelay = std::max(system.energyRegenDelay, kEnergyRegenDelay);
    }

    // durationHours > 0 implies a game-time duration effect, overriding the default "instant".
    const bool isGameTime = (effect.durationHours > 0.0f);

    if (!isGameTime && policy == DurationPolicy::Instant) {
        applyModifierToBase(set, attrId, op, effect.magnitude);
        routeDamageMeta(set);
        clampBaseVitals(set);
        recomputeCurrent(set, system);
        return true;
    }
    const f32 remainingSeconds = isGameTime
        ? effect.durationHours * 3600.0f
        : effect.durationSeconds;

    const u32 effectId = system.nextEffectId++;

    // Primary effect.
    ActiveEffect active;
    active.attribute = attrId;
    active.op = op;
    active.magnitude = effect.magnitude;
    active.infinite = (policy == DurationPolicy::Infinite);
    active.remaining = remainingSeconds;
    active.period = policy == DurationPolicy::Periodic ? effect.period : 0.0f;
    active.decayOnExpiry = (effect.expiryMode == "decay");
    active.decayPerHour  = effect.decayPerHour;
    active.expiryMagnitude = effect.expiryMagnitude;
    active.gameTime = isGameTime;
    active.effectId = effectId;
    if (!effect.grantedTag.empty()) {
        if (const auto tag = registry.find(effect.grantedTag)) {
            active.grantedTag = *tag;
            system.tags.add(*tag, registry);
        }
    }
    system.activeEffects.push_back(active);

    // Optional second attribute (e.g. affliction attribute malus).
    if (!effect.attribute2.empty()) {
        ActiveEffect active2;
        active2.attribute = attr(effect.attribute2);
        active2.op = parseOp(effect.op); // inherit op from primary
        active2.magnitude = effect.magnitude2;
        active2.infinite = active.infinite;
        active2.remaining = remainingSeconds;
        active2.period = 0.0f;
        active2.gameTime = isGameTime;
        active2.effectId = system.nextEffectId++;
        active2.grantedTag = active.grantedTag; // same tag for grouped removal
        if (active.grantedTag.isValid()) {
            system.tags.add(active.grantedTag, registry); // extra ref-count
        }
        system.activeEffects.push_back(active2);
    }

    if (outEffectId) {
        *outEffectId = effectId;
    }

    recomputeCurrent(set, system);
    return true;
}

void tickEffects(AttributeSet& set, AbilitySystem& system, f32 dt,
                 const GameplayTagRegistry& registry) {
    for (ActiveEffect& active : system.activeEffects) {
        if (active.gameTime) {
            continue; // ticked by tickGameTimeEffects
        }
        if (active.period > 0.0f) {
            active.sinceLastTick += dt;
            while (active.sinceLastTick >= active.period) {
                applyModifierToBase(set, active.attribute, active.op,
                                    active.magnitude);
                routeDamageMeta(set);
                clampBaseVitals(set);
                active.sinceLastTick -= active.period;
            }
        }
        if (!active.infinite) {
            active.remaining -= dt;
        }
    }

    for (const ActiveEffect& active : system.activeEffects) {
        if (active.gameTime) continue;
        const bool expired = !active.infinite && active.remaining <= 0.0f;
        if (expired) {
            expireEffect(system, active, registry);
        }
    }
    std::erase_if(system.activeEffects, [](const ActiveEffect& active) {
        return !active.gameTime && !active.infinite && active.remaining <= 0.0f;
    });

    recomputeCurrent(set, system);
}

void updateExhaustion(const AttributeSet& set, AbilitySystem& system,
                      const GameplayTagRegistry& registry, f32 recoverFraction) {
    const auto exhausted = registry.find("State.Exhausted");
    if (!exhausted) {
        return; // gate not in this scene's vocabulary → nothing to do
    }
    const f32 maxEnergy = currentValueOf(system, attr("maxEnergy"));
    const bool has = system.tags.has(*exhausted);
    if (!has && set.energy <= 0.0f) {
        system.tags.add(*exhausted, registry);
    } else if (has && set.energy >= maxEnergy * recoverFraction) {
        system.tags.remove(*exhausted, registry);
    }
}

void tickGameTimeEffects(AttributeSet& set, AbilitySystem& system, f64 gameDt,
                         const GameplayTagRegistry& registry) {
    const f32 gameDtF = static_cast<f32>(gameDt);
    for (ActiveEffect& active : system.activeEffects) {
        if (!active.gameTime) {
            continue; // ticked by tickEffects
        }
        if (!active.infinite) {
            active.remaining -= gameDtF;
        }
    }

    for (const ActiveEffect& active : system.activeEffects) {
        if (!active.gameTime) continue;
        const bool expired = !active.infinite && active.remaining <= 0.0f;
        if (expired) {
            expireEffect(system, active, registry);
        }
    }
    std::erase_if(system.activeEffects, [](const ActiveEffect& active) {
        return active.gameTime && !active.infinite && active.remaining <= 0.0f;
    });

    recomputeCurrent(set, system);
}

void removeEffectsByGrantedTag(AbilitySystem& system, GameplayTag tag,
                               const GameplayTagRegistry& registry) {
    if (!tag.isValid()) return;
    for (const ActiveEffect& active : system.activeEffects) {
        if (active.grantedTag == tag) {
            system.tags.remove(tag, registry); // one ref per effect
        }
    }
    std::erase_if(system.activeEffects,
                  [tag](const ActiveEffect& a) { return a.grantedTag == tag; });
}

void removeEffectById(AbilitySystem& system, u32 effectId,
                      const GameplayTagRegistry& registry) {
    if (effectId == 0) return;
    for (const ActiveEffect& active : system.activeEffects) {
        if (active.effectId == effectId) {
            expireEffect(system, active, registry);
        }
    }
    std::erase_if(system.activeEffects,
                  [effectId](const ActiveEffect& a) { return a.effectId == effectId; });
}

} // namespace gameplay
