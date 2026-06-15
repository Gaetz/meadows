#include "gameplay/ability/GameplayEffects.hpp"

#include <algorithm>

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

// Aggregates the non-periodic effect modifiers targeting `id` onto `base`:
// (base + Σadd)·Πmult, with Override winning. The shared core of recompute.
f32 aggregateModifiers(const AbilitySystem& system, u32 id, f32 base) {
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

} // namespace

void recomputeCurrent(AbilitySystem& system, std::span<const AttrSetRef> sets,
                      const DerivedStatRegistry* derived) {
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
            system.current[field.id] =
                aggregateModifiers(system, field.id, readF32(field, ref.instance));
        }
    }

    // Pass 2 — derived fields: aggregate over the formula. Sources (the nine
    // attributes) are non-derived, already computed in pass 1.
    const StatView view { system };
    for (const DerivedStat* stat : applied) {
        system.current[stat->target] =
            aggregateModifiers(system, stat->target, stat->formula(view));
    }

    clampVitalsCurrent(system);
}

void recomputeCurrent(const AttributeSet& set, AbilitySystem& system) {
    const AttrSetRef one { &AttributeSet::staticTypeInfo(), &set };
    recomputeCurrent(system, std::span<const AttrSetRef> { &one, 1 }, nullptr);
}

bool applyEffect(AttributeSet& set, AbilitySystem& system,
                 const EffectForm& effect, const GameplayTagRegistry& registry) {
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

    const u32 attrId = attr(effect.attribute);
    const ModifierOp op = parseOp(effect.op);
    const DurationPolicy policy = parseDuration(effect.duration);

    if (policy == DurationPolicy::Instant) {
        applyModifierToBase(set, attrId, op, effect.magnitude);
        routeDamageMeta(set);
        clampBaseVitals(set);
        recomputeCurrent(set, system);
        return true;
    }

    ActiveEffect active;
    active.attribute = attrId;
    active.op = op;
    active.magnitude = effect.magnitude;
    active.infinite = (policy == DurationPolicy::Infinite);
    active.remaining = effect.durationSeconds;
    active.period =
        policy == DurationPolicy::Periodic ? effect.period : 0.0f;
    if (!effect.grantedTag.empty()) {
        if (const auto tag = registry.find(effect.grantedTag)) {
            active.grantedTag = *tag;
            system.tags.add(*tag, registry);
        }
    }
    system.activeEffects.push_back(active);
    recomputeCurrent(set, system);
    return true;
}

void tickEffects(AttributeSet& set, AbilitySystem& system, f32 dt,
                 const GameplayTagRegistry& registry) {
    for (ActiveEffect& active : system.activeEffects) {
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
        const bool expired = !active.infinite && active.remaining <= 0.0f;
        if (expired && active.grantedTag.isValid()) {
            system.tags.remove(active.grantedTag, registry);
        }
    }
    std::erase_if(system.activeEffects, [](const ActiveEffect& active) {
        return !active.infinite && active.remaining <= 0.0f;
    });

    recomputeCurrent(set, system);
}

} // namespace gameplay
