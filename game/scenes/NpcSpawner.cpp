#include "game/scenes/NpcSpawner.hpp"

#include "data/forms/CoreForms.hpp"       // data::ActorForm
#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"       // data::childrenOf
#include "engine/assets/AssetDatabase.hpp"
#include "engine/assets/MeshData.hpp"     // render::SkinnedVertex
#include "engine/core/Log.hpp"
#include "engine/render/landscape/TerrainNoise.hpp" // terrain::height
#include "engine/rhi/Device.hpp"
#include "game/scenes/NpcDirector.hpp"    // Npc, NpcContext
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/actors/CharacterForms.hpp" // gameplay::ActorTagForm
#include "gameplay/actors/FollowerForms.hpp"  // FollowerClassForm
#include "gameplay/condition/Condition.hpp"   // transition condition gates
#include "world/ai/Perception.hpp"      // every built NPC perceives
#include "world/scene/AnimBridge.hpp"     // resolveActorVisual, buildAnimGraph
#include "world/scene/Components.hpp"

namespace game {

const RigData* NpcSpawner::loadRig(const NpcContext& ctx,
                                   const core::Guid& asset) {
    if (const auto it = rigCache.find(asset); it != rigCache.end()) {
        return it->second.skeleton.joints.empty() ? nullptr : &it->second;
    }
    RigData& rig = rigCache[asset]; // empty entry = negative cache
    const auto path = ctx.assetDb.resolve(asset);
    if (!path) {
        LOG_WARN("no asset registered for rig {}", asset.toString());
        return nullptr;
    }
    auto skeleton = assets::loadGltfSkeleton(*path);
    if (!skeleton) {
        return nullptr;
    }
    rig.skeleton = std::move(*skeleton);
    rig.clips = assets::loadGltfAnimations(*path, rig.skeleton);
    LOG_INFO("rig {} loaded — {} joints, {} clips", path->string(),
             rig.skeleton.joints.size(), rig.clips.size());
    return &rig;
}

void NpcSpawner::destroyNpc(rhi::Device& device, Npc& npc) {
    npc.anim.reset(); // references npc.graph — release first
    // The per-entity DRAW state (palette SSBO, model UBO, groups)
    // is renderer-owned now, swept there when this id leaves the snapshot.
    device.destroyBuffer(npc.indices);
    device.destroyBuffer(npc.vertices);
}

void NpcSpawner::refreshNpcs(
    rhi::Device& device, const NpcContext& ctx,
    const std::function<void(ecs::Entity, const core::Guid&)>&
        finalizeActorSpawn,
    vector<uptr<Npc>>& npcs, std::unordered_map<u64, Npc*>& npcByEntity,
    vector<Vec3>& patrolPoints, Vec3& characterSpot) {
    data::FormDatabase& forms = ctx.forms;
    // Prune NPCs whose entity was unloaded with its cell.
    for (auto it = npcs.begin(); it != npcs.end();) {
        if (!(*it)->entity.is_alive()) {
            destroyNpc(device, **it);
            it = npcs.erase(it);
        } else {
            ++it;
        }
    }

    // Patrol points: every LOADED "patrol" marker, grounded, spawn order.
    patrolPoints.clear();
    ctx.world.handle()
        .query<world::Transform, const world::MarkerKind>()
        .each([&](flecs::entity, world::Transform& transform,
                  const world::MarkerKind& marker) {
            if (marker.kind == "patrol") {
                if (!ctx.interiorMode) {
                    transform.position.y = render::terrain::height(
                        ctx.terrainParams, transform.position.x,
                        transform.position.z);
                }
                patrolPoints.push_back(transform.position);
            }
        });

    // Adding a component inside .each() is a structural change on a LOCKED
    // table (flecs LOCKED_STORAGE assert) — collect here, apply after.
    vector<std::pair<ecs::Entity, core::Guid>> pendingLoadouts;
    ctx.world.handle()
        .query<world::Transform, const world::RefId>()
        .each([&](flecs::entity e, world::Transform& transform,
                  const world::RefId& ref) {
            ecs::Entity entity { e };
            if (!entity.has<world::ActorMarker>() ||
                entity == ctx.playerEntity) {
                return;
            }
            for (const auto& tracked : npcs) {
                if (tracked->entity == entity) {
                    return; // already built
                }
            }
            const data::Form* base = forms.get(ref.base);
            const reflect::TypeInfo* type = forms.typeOf(ref.base);
            if (!base || !type ||
                !type->isA(data::ActorForm::staticTypeInfo().id)) {
                return;
            }
            const auto& actor = *static_cast<const data::ActorForm*>(base);
            const auto visual = world::resolveActorVisual(forms, actor);
            if (!visual) {
                return; // 2D/legacy actor (the Player has no appearance)
            }
            const RigData* rig = loadRig(ctx, visual->skeleton);
            if (!rig) {
                return;
            }
            const auto meshPath = ctx.assetDb.resolve(visual->mesh);
            auto skinned =
                meshPath ? assets::loadGltfSkinnedMesh(*meshPath)
                         : std::nullopt;
            if (!skinned) {
                return;
            }
            auto graph = world::buildAnimGraph(
                forms, visual->animGraph,
                [&](const core::Guid& asset,
                    const str& name) -> std::optional<anim::AnimClip> {
                    const RigData* clipRig = loadRig(ctx, asset);
                    if (!clipRig) {
                        return std::nullopt;
                    }
                    for (const assets::GltfClip& clip : clipRig->clips) {
                        if (name.empty() || clip.name == name) {
                            return clip.clip;
                        }
                    }
                    LOG_WARN("no animation '{}' in rig {}", name,
                             asset.toString());
                    return std::nullopt;
                });
            if (!graph) {
                return;
            }

            auto npc = std::make_unique<Npc>();
            npc->entity = entity;
            npc->editorId = actor.editorId; // logs/debug name the NPC
            npc->rig = rig;
            npc->graph = std::move(*graph);
            npc->anim = std::make_unique<anim::GraphInstance>(npc->graph);
            // The daily routine + the anim tag gates
            // (sitting from furniture use, dead from the GAS life state).
            npc->schedule = actor.schedule;
            npc->courage = actor.courage; // Flees below (1 - courage)
            npc->age = actor.age; // Per-tick age mods (0 = ageless)
            // The class combat style steers the special-power use
            // (and the healer's stand-off band) in NpcCombatController.
            if (actor.followerClass.isValid()) {
                if (const auto* followerClass =
                        forms.find<gameplay::FollowerClassForm>(
                            actor.followerClass)) {
                    npc->combatStyle = followerClass->combatStyle;
                }
            }
            // Brain script: Lua decides this actor's combat moves
            // (docs/BOSS-SCRIPTING.md); keyed by the FORM — every
            // instance shares one compiled decide.
            npc->brainScript = actor.brainScript;
            npc->brainKey = actor.id;
            // ActorTagForm children become REAL gameplay tags
            // on the actor's system (registerTag is idempotent and
            // auto-registers ancestors); the first Faction.* tag is what
            // quests/crime filter deaths by. Mutating the EXISTING
            // AbilitySystem component inside .each is safe (no table move).
            data::childrenOf<gameplay::ActorTagForm>(
                forms, actor.id, [&](const gameplay::ActorTagForm& tagForm) {
                    const gameplay::GameplayTag tag =
                        ctx.gameTags.registerTag(tagForm.tag);
                    if (entity.has<gameplay::AbilitySystem>()) {
                        entity.get_mut<gameplay::AbilitySystem>().tags.add(
                            tag, ctx.gameTags);
                    }
                    if (!npc->factionTag.isValid() &&
                        tagForm.tag.starts_with("Faction.")) {
                        npc->factionTag = tag;
                    }
                    if (tagForm.tag == "Faction.Bandits") {
                        npc->hostile = true;
                    }
                    if (tagForm.tag == "Faction.VillageGuard") {
                        npc->guard = true; // D2: aggro only while Wanted
                    }
                });
            npc->anim->setTagCheck(
                [raw = npc.get()](std::string_view tag) {
                    if (raw->sitting && tag == raw->sitGate) {
                        return true; // D1: the claimed point's animTag
                    }
                    if (tag == "State.Dead") {
                        return raw->dead;
                    }
                    if (tag == "State.Attacking") { // swing gate
                        return raw->attacking;
                    }
                    return false;
                });
            // Full condition-evaluator gate on transitions:
            // the opaque ref is the transition's guid, its
            // ConditionForm children evaluate against THIS actor. forms and
            // gameTags are scene members outliving every Npc. Lua clauses
            // stay unwired here (fail closed) until a brain-VM need shows.
            npc->anim->setConditionCheck(
                [raw = npc.get(), &conditionForms = ctx.forms,
                 &gameTags = ctx.gameTags](std::string_view ref) {
                    const auto conditionId = core::Guid::fromString(ref);
                    if (!conditionId || !raw->entity.is_alive()) {
                        return false;
                    }
                    gameplay::EvalContext context;
                    context.tags = &gameTags;
                    if (raw->entity.has<gameplay::AbilitySystem>()) {
                        context.abilitySystem =
                            &raw->entity.get<gameplay::AbilitySystem>();
                    }
                    if (raw->entity.has<gameplay::Inventory>()) {
                        context.inventory =
                            &raw->entity.get<gameplay::Inventory>();
                    }
                    return gameplay::conditionsPass(conditionForms,
                                                    *conditionId, context);
                });
            // The sink FINALLY gets a runtime consumer —
            // events buffer on the Npc (uptr = stable address) and drain
            // onto the EventBus in update(), where the context lives.
            npc->anim->setEventSink(
                [raw = npc.get()](std::string_view name) {
                    raw->pendingAnimEvents.emplace_back(name);
                });
            // The sword hand (UAL rig: "hand_r"); -1 = no weapon shown.
            npc->handJoint = rig->skeleton.findJoint("hand_r");
            npc->tint = visual->tint;
            // The pose is normally written by update(); a paused sim (boot
            // = Spectator) extracts BEFORE any update, and the weapon
            // attach walks the pose — it must be skeleton-sized from birth.
            anim::bindPose(rig->skeleton, npc->pose);
            npc->palette.assign(rig->skeleton.joints.size(), Mat4 { 1.0f });
            npc->vertices = device.createBuffer(
                { .usage = rhi::BufferUsage::Vertex,
                  .size = skinned->vertices.size() *
                          sizeof(render::SkinnedVertex) },
                skinned->vertices.data());
            npc->indices = device.createBuffer(
                { .usage = rhi::BufferUsage::Index,
                  .size = skinned->indices.size() * sizeof(u32) },
                skinned->indices.data());
            npc->indexCount = static_cast<u32>(skinned->indices.size());

            // Ground the entity (actors have no MeshRender: the mesh-path
            // snap skipped them). Interiors keep the authored ABSOLUTE y —
            // the overworld terrain height would teleport the actor out of
            // the dungeon.
            if (!ctx.interiorMode) {
                transform.position.y = render::terrain::height(
                    ctx.terrainParams, transform.position.x,
                    transform.position.z);
            }
            // Stats + saved state / loadout run through
            // finalizeActorSpawn — deferred below: it adds components, a
            // table move on the locked iteration.
            pendingLoadouts.emplace_back(entity, actor.id);

            if (npcs.empty()) {
                characterSpot = transform.position;
            }
            npcs.push_back(std::move(npc));
            LOG_INFO("NPC '{}' built from Forms", actor.editorId);
        });
    for (auto& [entity, actorId] : pendingLoadouts) {
        // Every built NPC perceives (a reload keeps a saved state
        // through the reflected component; a fresh one is Calm). ADDING
        // the component is a table move — it must run out here with the
        // loadouts, never inside the locked .each above (flecs
        // LOCKED_STORAGE: fatal in Debug, silent corruption in Release).
        if (!entity.has<world::Perception>()) {
            entity.set<world::Perception>({});
        }
        finalizeActorSpawn(entity, actorId);
    }
    // Seed the death flag from the (possibly restored) life
    // state, so a corpse reloaded from a save or a cell re-entry never fires a
    // spurious OnDeath edge on its first tick.
    if (const auto deadTag = ctx.gameTags.find("State.Dead")) {
        for (auto& npcPtr : npcs) {
            if (npcPtr->entity.is_alive() &&
                npcPtr->entity.has<gameplay::AbilitySystem>()) {
                npcPtr->dead =
                    npcPtr->entity.get<gameplay::AbilitySystem>().tags.has(
                        *deadTag);
            }
        }
    }
    // Same seeding for the downed mirror — a follower
    // reloaded mid-bleedout (applySavedState re-derived State.Downed)
    // must not fire a spurious downed edge that would RESET the saved
    // bleedout clock (CombatState.downedSeconds is captured state).
    if (const auto downedTag = ctx.gameTags.find("State.Downed")) {
        for (auto& npcPtr : npcs) {
            if (npcPtr->entity.is_alive() &&
                npcPtr->entity.has<gameplay::AbilitySystem>()) {
                npcPtr->downed =
                    npcPtr->entity.get<gameplay::AbilitySystem>().tags.has(
                        *downedTag);
            }
        }
    }
    // Refresh the entity->Npc map (this is the only place npcs
    // changes; the uptr targets keep their addresses).
    npcByEntity.clear();
    for (auto& npcPtr : npcs) {
        npcByEntity[npcPtr->entity.id()] = npcPtr.get();
    }
}

} // namespace game
