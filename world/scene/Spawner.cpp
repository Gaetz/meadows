#include "world/scene/Spawner.hpp"

#include "engine/core/Log.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/stats/Afflictions.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/Drugs.hpp"
#include "gameplay/stats/Injuries.hpp"
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/ResonanceDecays.hpp"
#include "gameplay/stats/StatusBuildup.hpp"
#include "gameplay/stats/Survival.hpp"

namespace world {

namespace {

void spawnStatic(SpawnContext&, ecs::Entity entity, const data::Form&,
                 const reflect::TypeInfo&) {
    entity.add<StaticMarker>();
}

void spawnItem(SpawnContext&, ecs::Entity entity, const data::Form&,
               const reflect::TypeInfo&) {
    entity.add<ItemMarker>();
}

void spawnActor(SpawnContext&, ecs::Entity entity, const data::Form& base,
                const reflect::TypeInfo& baseType) {
    entity.add<ActorMarker>();

    // Mandatory GAS for actors (§2.7): an AttributeSet seeded from the base
    // form's reflected `maxHealth` (through reflection — no per-type code), plus
    // an AbilitySystem with its current-value overlay initialized.
    gameplay::AttributeSet attributes;
    if (const reflect::FieldInfo* field = baseType.findField("maxHealth");
        field && field->kind == reflect::FieldKind::F32) {
        const reflect::Value value = field->get(&base);
        if (const f32* maxHealth = std::get_if<f32>(&value)) {
            gameplay::setBaseValue(attributes, gameplay::attr("maxHealth"),
                                   *maxHealth);
            gameplay::setBaseValue(attributes, gameplay::attr("health"),
                                   *maxHealth);
        }
    }
    gameplay::AbilitySystem system;
    gameplay::initializeCurrent(system, attributes);

    entity.set<gameplay::AttributeSet>(attributes);
    entity.set<gameplay::AbilitySystem>(system);

    // Full character-stats components (§2.7): all actors carry the complete
    // stat sheet so tickCharacter works uniformly across player and NPCs.
    entity.set<gameplay::CoreAttributes>({});
    entity.set<gameplay::Resonance>({});
    entity.set<gameplay::Survival>({});
    entity.set<gameplay::StatusBuildup>({});
    entity.set<gameplay::CombatState>({});
    entity.set<gameplay::Injuries>({});
    entity.set<gameplay::Afflictions>({});
    entity.set<gameplay::ActiveDrugs>({});
    entity.set<gameplay::ResonanceDecays>({});
}

} // namespace

ecs::Entity Spawner::spawn(SpawnContext& ctx, const ReferenceForm& reference,
                           ecs::Entity cellEntity) const {
    const data::FormHandle baseHandle = ctx.forms.handleOf(reference.baseForm);
    const data::Form* base = ctx.forms.get(baseHandle);
    const reflect::TypeInfo* baseType = ctx.forms.typeOf(baseHandle);
    if (!base || !baseType) {
        LOG_WARN("Spawner: reference {} has no resolvable base form {}",
                 reference.id.toString(), reference.baseForm.toString());
        return ecs::Entity {};
    }

    const auto category = ctx.categories.categoryOf(baseType->id);
    if (!category) {
        LOG_WARN("Spawner: base form {} has no category, not spawnable",
                 baseType->name);
        return ecs::Entity {};
    }
    const auto spawnFn = byCategory.find(*category);
    if (spawnFn == byCategory.end()) {
        LOG_WARN("Spawner: no spawner registered for the category of {}",
                 baseType->name);
        return ecs::Entity {};
    }

    ecs::Entity entity = ctx.world.create();

    // Universal components, from the resolved instance data.
    Transform transform;
    transform.position = reference.position;
    transform.rotation = reference.rotation;
    transform.scale = reference.scale;
    entity.set<Transform>(transform);

    RefId refId;
    refId.referenceId = reference.id;
    refId.base = baseHandle;
    refId.cell = ctx.forms.handleOf(reference.cell);
    entity.set<RefId>(refId);

    // SpriteRender seeded from the base form's reflected `sprite` field — §2.7
    // "through reflection": works for any base form that declares one, with no
    // per-type code (WeaponForm, ActorForm, ...).
    if (const reflect::FieldInfo* spriteField = baseType->findField("sprite");
        spriteField && spriteField->kind == reflect::FieldKind::Guid) {
        SpriteRender sprite;
        sprite.sprite = std::get<core::Guid>(spriteField->get(base));
        entity.set<SpriteRender>(sprite);
    }

    // Runtime cell membership (ephemeral; never serialized).
    if (cellEntity.is_valid()) {
        entity.add<ecs::InCell>(cellEntity);
    }

    // Category-specific wiring.
    spawnFn->second(ctx, entity, *base, *baseType);
    return entity;
}

void registerCoreSpawners(Spawner& spawner) {
    spawner.registerCategory(FormCategory::Static, &spawnStatic);
    spawner.registerCategory(FormCategory::Item, &spawnItem);
    spawner.registerCategory(FormCategory::Actor, &spawnActor);
}

} // namespace world
