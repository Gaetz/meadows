#include "gameplay/ability/AbilitySystem.hpp"

#include "engine/ecs/World.hpp"
#include "gameplay/combat/MeleeSwing.hpp"

namespace gameplay {

void registerGameplayComponents(ecs::World& world) {
    world.registerComponent<AttributeSet>();   // reflected: base values serialize
    world.handle().component<AbilitySystem>(); // runtime-only state
    world.handle().component<MeleeSwing>();    // runtime-only (P0 A3)
}

std::optional<f32> baseValueOf(const AttributeSet& set, u32 attrId) {
    const reflect::FieldInfo* field =
        AttributeSet::staticTypeInfo().findField(attrId);
    if (!field || field->kind != reflect::FieldKind::F32) {
        return std::nullopt;
    }
    const reflect::Value value = field->get(&set);
    const f32* result = std::get_if<f32>(&value);
    return result ? std::optional<f32> { *result } : std::nullopt;
}

bool setBaseValue(AttributeSet& set, u32 attrId, f32 value) {
    const reflect::FieldInfo* field =
        AttributeSet::staticTypeInfo().findField(attrId);
    if (!field || field->kind != reflect::FieldKind::F32) {
        return false;
    }
    return field->set(&set, reflect::Value { value });
}

void initializeCurrent(AbilitySystem& system, const AttributeSet& set) {
    for (const reflect::FieldInfo& field : AttributeSet::staticTypeInfo().fields) {
        if (field.kind != reflect::FieldKind::F32) {
            continue;
        }
        const reflect::Value value = field.get(&set);
        if (const f32* result = std::get_if<f32>(&value)) {
            system.current[field.id] = *result;
        }
    }
}

f32 currentValueOf(const AbilitySystem& system, u32 attrId) {
    const auto it = system.current.find(attrId);
    return it != system.current.end() ? it->second : 0.0f;
}

void syncStateTag(AbilitySystem& system, const GameplayTagRegistry& registry,
                  std::string_view name, bool on) {
    const auto tag = registry.find(name);
    if (!tag) {
        return;
    }
    const bool tagged = system.tags.has(*tag);
    if (on && !tagged) {
        system.tags.add(*tag, registry);
    } else if (!on && tagged) {
        system.tags.remove(*tag, registry);
    }
}

} // namespace gameplay
