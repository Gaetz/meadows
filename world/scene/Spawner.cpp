#include "world/scene/Spawner.hpp"

#include <glm/gtc/quaternion.hpp>

#include "data/forms/FormQuery.hpp"
#include "data/forms/VisualForms.hpp"
#include "engine/core/Log.hpp"
#include "gameplay/interaction/FurnitureForms.hpp"
#include "world/worldspace/WorldForms.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"
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

void spawnLight(SpawnContext&, ecs::Entity entity, const data::Form& base,
                const reflect::TypeInfo&) {
    entity.add<LightMarker>();
    const auto& light = static_cast<const data::LightForm&>(base);
    LightSource source;
    source.color = light.color;
    source.intensity = light.intensity;
    source.radius = light.radius;
    source.spotAngle = light.kind == "spot" ? light.spotAngle : 0.0f;
    source.flicker = light.flicker;
    source.shaft = light.shaft;
    source.shaftLength = light.shaftLength;
    source.shaftSoftness = light.shaftSoftness;
    source.dustDensity = light.dustDensity;
    source.sunLinked = light.sunLinked;
    source.castsShadow =
        light.castsShadow || light.shadowMode == "key";
    source.rcOnly = light.shadowMode == "rcOnly";
    source.windowHalfWidth = light.windowHalfWidth;
    source.windowHalfHeight = light.windowHalfHeight;
    entity.set<LightSource>(source);
}

void spawnWaterVolume(SpawnContext&, ecs::Entity entity,
                      const data::Form& base, const reflect::TypeInfo&) {
    const auto& form = static_cast<const data::WaterVolumeForm&>(base);
    entity.set<WaterVolume>({ form.halfExtents, form.tint, form.chop });
}

void spawnMarker(SpawnContext&, ecs::Entity entity, const data::Form& base,
                 const reflect::TypeInfo&) {
    const auto& marker = static_cast<const MarkerForm&>(base);
    entity.set<MarkerKind>({ marker.kind });
}

// Doors get their visual from the universal model/material
// wiring; the category hook only adds the travel target.
void spawnDoor(SpawnContext&, ecs::Entity entity, const data::Form& base,
               const reflect::TypeInfo&) {
    const auto& door = static_cast<const DoorForm&>(base);
    entity.set<DoorTarget>({ door.targetMarker });
}

void spawnTrigger(SpawnContext&, ecs::Entity entity, const data::Form& base,
                  const reflect::TypeInfo&) {
    entity.add<TriggerMarker>();
    const auto& trigger = static_cast<const TriggerForm&>(base);
    TriggerVolume volume;
    volume.halfExtents = trigger.halfExtents;
    volume.event = trigger.event;
    volume.script = trigger.script;
    volume.once = trigger.once;
    entity.set<TriggerVolume>(volume);
}

void spawnFurniture(SpawnContext&, ecs::Entity entity,
                    const data::Form& base, const reflect::TypeInfo&) {
    entity.add<FurnitureMarker>();
    entity.set<FurnitureRef>({ base.id });
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
            // STATS.md balance override (`override ?? formula`): a
            // POSITIVE authored max pins the derived maxHealth — a
            // bandit really has its authored 35. 0 = the attribute
            // formula (the Player).
            gameplay::setBaseValue(attributes,
                                   gameplay::attr("maxHealthOverride"),
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

    // Prefab expansion (H8): a placed reference whose base is a PrefabForm
    // spawns every template child with a DERIVED, deterministic guid —
    // combine(instance, template) — so saves/patches can target one child
    // of one placed prefab forever. Nested prefabs recurse naturally.
    if (*category == FormCategory::Prefab) {
        ecs::Entity root = ctx.world.create();
        Transform rootTransform;
        rootTransform.position = reference.position;
        rootTransform.rotation = reference.rotation;
        rootTransform.scale = reference.scale;
        root.set<Transform>(rootTransform);
        RefId rootId;
        rootId.referenceId = reference.id;
        rootId.base = baseHandle;
        rootId.cell = ctx.forms.handleOf(reference.cell);
        root.set<RefId>(rootId);
        root.add<PrefabRootMarker>();
        if (cellEntity.is_valid()) {
            root.add<ecs::InCell>(cellEntity);
        }
        data::forEach<ReferenceForm>(
            ctx.forms, [&](const ReferenceForm& child) {
                if (child.prefab != base->id || !child.enabled) {
                    return;
                }
                ReferenceForm derived = child;
                derived.id = core::Guid::combine(reference.id, child.id);
                // A SAVE materializes a touched derived
                // child as a real record under this same guid (a patch to
                // a record no plugin creates would be dropped as an
                // orphan). When that record exists in the database, IT is
                // the truth — the template expansion steps aside (the
                // record spawns through the normal cell path, disabled or
                // moved as saved).
                if (ctx.forms.handleOf(derived.id).isValid()) {
                    return;
                }
                if (ctx.filter && !ctx.filter(derived.id)) {
                    return; // pending layer says gone (picked up)
                }
                derived.prefab = {}; // the derived copy is a real placement
                derived.cell = reference.cell;
                // Compose the instance transform over the template's
                // (template transforms are relative to the prefab pivot).
                derived.position =
                    reference.position +
                    reference.rotation *
                        (child.position * reference.scale);
                derived.rotation = reference.rotation * child.rotation;
                derived.scale = reference.scale * child.scale;
                spawn(ctx, derived, cellEntity);
            });
        return root;
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

    // MeshRender seeded the same reflected way from `model` + `material`
    // (H8): any base form declaring them gets a 3D visual — StaticForm,
    // FurnitureForm today, ActorForm's appearance path later.
    if (const reflect::FieldInfo* modelField = baseType->findField("model");
        modelField && modelField->kind == reflect::FieldKind::Guid) {
        const core::Guid model =
            std::get<core::Guid>(modelField->get(base));
        if (model.isValid()) {
            MeshRender mesh;
            mesh.model = model;
            if (const reflect::FieldInfo* materialField =
                    baseType->findField("material");
                materialField &&
                materialField->kind == reflect::FieldKind::Guid) {
                mesh.material =
                    std::get<core::Guid>(materialField->get(base));
            }
            entity.set<MeshRender>(mesh);
        }
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
    spawner.registerCategory(FormCategory::Light, &spawnLight);
    spawner.registerCategory(FormCategory::Water, &spawnWaterVolume);
    spawner.registerCategory(FormCategory::Marker, &spawnMarker);
    spawner.registerCategory(FormCategory::Trigger, &spawnTrigger);
    spawner.registerCategory(FormCategory::Furniture, &spawnFurniture);
    spawner.registerCategory(FormCategory::Door, &spawnDoor);
    // FormCategory::Prefab is handled inside Spawner::spawn (expansion).
}

} // namespace world
