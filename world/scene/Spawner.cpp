#include "world/scene/Spawner.hpp"

#include "engine/core/Log.hpp"

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

void spawnActor(SpawnContext&, ecs::Entity entity, const data::Form&,
                const reflect::TypeInfo&) {
    entity.add<ActorMarker>();
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
