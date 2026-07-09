#include "game/scenes/NpcDirector.hpp"

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "data/forms/CoreForms.hpp"       // data::ActorForm, WeaponForm
#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"       // data::childrenOf
#include "engine/assets/AssetDatabase.hpp"
#include "engine/assets/GltfMesh.hpp"
#include "engine/core/Log.hpp"
#include "engine/physics/Physics.hpp"     // phys::PhysicsWorld/CharacterBody/RayHit
#include "engine/assets/MeshData.hpp"     // render::SkinnedVertex
#include "engine/render/landscape/TerrainNoise.hpp" // terrain::height
#include "engine/rhi/Device.hpp"
#include "game/SceneSubmit.hpp"           // RenderSnapshot (U4-2b extract)
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp" // attr, currentValueOf
#include "gameplay/actors/ActorState.hpp"
#include "gameplay/actors/CharacterForms.hpp"
#include "gameplay/actors/CharacterTick.hpp"
#include "gameplay/ai/AiForms.hpp"
#include "gameplay/ai/ScheduleSystem.hpp"
#include "gameplay/event/EventBus.hpp"
#include "gameplay/interaction/Furniture.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/EquipmentStats.hpp" // weaponDamageEvent
#include "gameplay/stats/GameClock.hpp"
#include "world/ai/TerrainNavigator.hpp"
#include "world/scene/AnimBridge.hpp"     // resolveActorVisual, buildAnimGraph
#include "world/scene/Components.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace game {

namespace {

// U4-7: the stat-space -> world mapping and the NPC gait now come from
// StatsTuningForm (§5 moddable) — the same scale the player uses, no more
// hand-mirrored copy.

} // namespace

const RigData* NpcDirector::loadRig(const NpcContext& ctx,
                                    const core::Guid& asset) {
    if (const auto it = rigCache.find(asset); it != rigCache.end()) {
        return it->second.skeleton.joints.empty() ? nullptr : &it->second;
    }
    RigData& rig = rigCache[asset]; // empty entry = negative cache
    const auto path = ctx.assetDb.resolve(asset);
    if (!path) {
        LOG_WARN("B6: no asset registered for rig {}", asset.toString());
        return nullptr;
    }
    auto skeleton = assets::loadGltfSkeleton(*path);
    if (!skeleton) {
        return nullptr;
    }
    rig.skeleton = std::move(*skeleton);
    rig.clips = assets::loadGltfAnimations(*path, rig.skeleton);
    LOG_INFO("B6: rig {} loaded — {} joints, {} clips", path->string(),
             rig.skeleton.joints.size(), rig.clips.size());
    return &rig;
}

void NpcDirector::destroyNpc(rhi::Device& device, Npc& npc) {
    npc.anim.reset(); // references npc.graph — release first
    // U4-2b: the per-entity DRAW state (palette SSBO, model UBO, groups)
    // is renderer-owned now, swept there when this id leaves the snapshot.
    device.destroyBuffer(npc.indices);
    device.destroyBuffer(npc.vertices);
}

void NpcDirector::refreshNpcs(
    rhi::Device& device, const NpcContext& ctx,
    const std::function<void(ecs::Entity, const core::Guid&)>&
        finalizeActorSpawn) {
    data::FormDatabase& forms = ctx.forms;
    // Prune NPCs whose entity was unloaded with its cell.
    for (auto it = npcs_.begin(); it != npcs_.end();) {
        if (!(*it)->entity.is_alive()) {
            destroyNpc(device, **it);
            it = npcs_.erase(it);
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
                transform.position.y = render::terrain::height(
                    ctx.terrainParams, transform.position.x,
                    transform.position.z);
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
            for (const auto& tracked : npcs_) {
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
                    LOG_WARN("B6: no animation '{}' in rig {}", name,
                             asset.toString());
                    return std::nullopt;
                });
            if (!graph) {
                return;
            }

            auto npc = std::make_unique<Npc>();
            npc->entity = entity;
            npc->rig = rig;
            npc->graph = std::move(*graph);
            npc->anim = std::make_unique<anim::GraphInstance>(npc->graph);
            // Chantier 3 B3/B6: the daily routine + the anim tag gates
            // (sitting from furniture use, dead from the GAS life state).
            npc->schedule = actor.schedule;
            // Chantier 6 A1: ActorTagForm children become REAL gameplay tags
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
                    if (tag == "State.Sitting") {
                        return raw->sitting;
                    }
                    if (tag == "State.Dead") {
                        return raw->dead;
                    }
                    return false;
                });
            npc->tint = visual->tint;
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

            // Ground the entity (actors have no MeshRender: the B1 snap
            // skipped them).
            transform.position.y = render::terrain::height(
                ctx.terrainParams, transform.position.x,
                transform.position.z);
            // Chantier 5 B3: stats + saved state / loadout run through
            // finalizeActorSpawn — deferred below: it adds components, a
            // table move on the locked iteration.
            pendingLoadouts.emplace_back(entity, actor.id);

            if (npcs_.empty()) {
                characterSpot_ = transform.position;
            }
            npcs_.push_back(std::move(npc));
            LOG_INFO("B6: NPC '{}' built from Forms",
                     static_cast<const data::ActorForm*>(forms.get(ref.base))
                         ->editorId);
        });
    for (auto& [entity, actorId] : pendingLoadouts) {
        finalizeActorSpawn(entity, actorId);
    }
    // Chantier 6 A1: seed the death flag from the (possibly restored) life
    // state, so a corpse reloaded from a save or a cell re-entry never fires a
    // spurious OnDeath edge on its first tick.
    if (const auto deadTag = ctx.gameTags.find("State.Dead")) {
        for (auto& npcPtr : npcs_) {
            if (npcPtr->entity.is_alive() &&
                npcPtr->entity.has<gameplay::AbilitySystem>()) {
                npcPtr->dead =
                    npcPtr->entity.get<gameplay::AbilitySystem>().tags.has(
                        *deadTag);
            }
        }
    }
}

// Chantier 3 B3: re-evaluate the schedule every 10 game minutes; execute the
// active package (travel / wander / useFurniture / guard...).
void NpcDirector::updateNpcSchedule(const NpcContext& ctx, Npc& npc,
                                    f32 hourOfDay) {
    const i32 slot = static_cast<i32>(hourOfDay * 6.0f);
    if (slot == npc.lastEvaluatedSlot) {
        return;
    }
    npc.lastEvaluatedSlot = slot;
    const auto intent =
        gameplay::evaluateSchedule(ctx.forms, npc.schedule, hourOfDay);
    const gameplay::AiPackageForm* next = intent ? intent->package : nullptr;
    const core::Guid nextLocation =
        intent ? intent->location : core::Guid {};
    if (next != npc.activePackage || nextLocation != npc.activeLocation) {
        // Package change: stand up, drop the path, release furniture.
        npc.activePackage = next;
        npc.activeLocation = nextLocation;
        npc.intentReason = intent ? intent->reason : "(no schedule entry)";
        npc.path.clear();
        npc.pathIndex = 0;
        npc.sitting = false;
        if (npc.furnitureClaimed) {
            ctx.furnitureOccupancy.release(npc.entity.id());
            npc.furnitureClaimed = false;
        }
    }
}

bool NpcDirector::moveNpcAlongPath(const NpcContext& ctx, Npc& npc, f32 dt,
                                   f32 speedScale) {
    if (npc.pathIndex >= npc.path.size()) {
        return true;
    }
    auto& transform = npc.entity.get_mut<world::Transform>();
    const auto& sys = npc.entity.get<gameplay::AbilitySystem>();
    const f32 walkSpeed =
        gameplay::currentValueOf(sys, gameplay::attr("movementSpeed")) *
        ctx.statsTuning.movementSpeedScale3D * ctx.statsTuning.npcWalkFactor *
        speedScale; // U4-7: §5-tunable

    const Vec3 goal = npc.path[npc.pathIndex];
    Vec3 to = goal - transform.position;
    to.y = 0.0f;
    const f32 distance = glm::length(to);
    if (distance < 0.35f) {
        ++npc.pathIndex;
        return npc.pathIndex >= npc.path.size();
    }
    const Vec3 dir = to / distance;
    transform.position += dir * glm::min(walkSpeed * dt, distance);
    transform.position.y = render::terrain::height(
        ctx.terrainParams, transform.position.x, transform.position.z);
    const f32 goalYaw = std::atan2(dir.x, dir.z);
    f32 delta = goalYaw - npc.yaw;
    while (delta > glm::pi<f32>()) {
        delta -= glm::two_pi<f32>();
    }
    while (delta < -glm::pi<f32>()) {
        delta += glm::two_pi<f32>();
    }
    npc.yaw += delta * (1.0f - std::exp(-8.0f * dt));
    transform.rotation = glm::angleAxis(npc.yaw, Vec3 { 0.0f, 1.0f, 0.0f });
    npc.speed += (walkSpeed - npc.speed) * (1.0f - std::exp(-10.0f * dt));
    return false;
}

void NpcDirector::update(f32 dt, const NpcContext& ctx) {
    const f32 hourOfDay =
        static_cast<f32>(std::fmod(ctx.gameClock.gameHours(), 24.0));
    const f64 gameDt =
        static_cast<f64>(dt) * static_cast<f64>(ctx.gameClock.timescale);
    const gameplay::CharacterTickContext tickCtx { ctx.derivedStats,
                                                   ctx.gameTags,
                                                   ctx.statsTuning };
    const auto deadTag = ctx.gameTags.find("State.Dead");
    for (auto& npcPtr : npcs_) {
        Npc& npc = *npcPtr;
        auto& transform = npc.entity.get_mut<world::Transform>();
        f32 idleDecay = 10.0f;

        // B6: NPCs run the full character pipeline too (effects, stagger, life
        // state) — that's where State.Dead comes from.
        gameplay::tickCharacter(npc.entity, dt, gameDt, tickCtx);
        const auto& npcSys = npc.entity.get<gameplay::AbilitySystem>();
        const bool wasDead = npc.dead;
        npc.dead = deadTag && npcSys.tags.has(*deadTag);
        // Chantier 6 A1: the live->dead EDGE is the gameplay event — quests
        // (kill tasks) and crime listen on the bus. Reload paths never fire
        // it: refreshNpcs seeds npc.dead from the tag.
        if (npc.dead && !wasDead) {
            ctx.eventBus.dispatch({ gameplay::eventKind("OnDeath"),
                                    ecs::Entity {}, npc.entity,
                                    npc.factionTag });
        }
        // (The corpse is lootable — its Inventory was rolled from the
        // LoadoutEntryForm children at build, chantier 4 B5.)
        if (npc.dead) {
            // The death transition (anim graph, State.Dead gate) plays; the
            // body stays. Despawn: a later slice.
            npc.sitting = false;
            npc.path.clear();
            npc.speed = 0.0f;
            npc.anim->setParam("speed", 0.0f);
            npc.anim->update(dt, 0.0f);
            anim::bindPose(npc.rig->skeleton, npc.pose);
            npc.anim->evaluate(npc.pose);
            anim::skinMatrices(npc.rig->skeleton, npc.pose, npc.palette);
            continue;
        }

        // B5: hostile actors hunt the player on sight (distance + a clear line
        // — the perception cone can refine later). D2: a guard turns hostile
        // while the player carries a bounty (tag-based — the relations table
        // stays a later pass).
        bool wanted = false;
        if (npc.guard && ctx.playerEntity.is_alive()) {
            if (const auto tag = ctx.gameTags.find("Crime.Wanted")) {
                wanted = ctx.playerEntity.get<gameplay::AbilitySystem>()
                             .tags.has(*tag);
            }
        }
        bool inCombat = false;
        if ((npc.hostile || wanted) && ctx.playMode && ctx.player) {
            const Vec3 playerPos = ctx.player->position();
            Vec3 to = playerPos - transform.position;
            to.y = 0.0f;
            const f32 distance = glm::length(to);
            if (distance < 16.0f) {
                const Vec3 eye =
                    transform.position + Vec3 { 0.0f, 1.5f, 0.0f };
                const Vec3 target = playerPos + Vec3 { 0.0f, 1.2f, 0.0f };
                const Vec3 dir = glm::normalize(target - eye);
                const f32 sight = glm::length(target - eye);
                const phys::RayHit hit = ctx.physics->rayCast(eye, dir, sight);
                const bool blocked = hit.hit && hit.distance < sight - 0.6f;
                if (!blocked) {
                    inCombat = true;
                    npc.sitting = false;
                    npc.attackCooldown -= dt;
                    npc.repathTimer -= dt;
                    if (distance > 1.8f) {
                        if (npc.repathTimer <= 0.0f) {
                            const nav::PathResult found =
                                ctx.navigator->findPath({ transform.position,
                                                          playerPos, 1.2f });
                            npc.path = found.success ? found.waypoints
                                                     : vector<Vec3> {};
                            npc.pathIndex = 0;
                            npc.repathTimer = 1.0f;
                        }
                        moveNpcAlongPath(ctx, npc, dt, 1.8f); // hurry
                    } else {
                        npc.path.clear();
                        // Face the player and swing.
                        const f32 goalYaw = std::atan2(to.x, to.z);
                        npc.yaw = goalYaw;
                        transform.rotation = glm::angleAxis(
                            npc.yaw, Vec3 { 0.0f, 1.0f, 0.0f });
                        if (npc.attackCooldown <= 0.0f && ctx.banditWeapon &&
                            ctx.playerEntity.is_alive() && !ctx.godMode) {
                            npc.attackCooldown = 1.6f;
                            gameplay::StatBlock block {
                                ctx.playerEntity
                                    .get_mut<gameplay::CoreAttributes>(),
                                ctx.playerEntity
                                    .get_mut<gameplay::AttributeSet>(),
                                ctx.playerEntity
                                    .get_mut<gameplay::AbilitySystem>(),
                                ctx.playerEntity
                                    .get_mut<gameplay::CombatState>()
                            };
                            const gameplay::DamageResult result =
                                gameplay::applyDamage(
                                    block,
                                    gameplay::weaponDamageEvent(
                                        *ctx.banditWeapon, npcSys),
                                    ctx.gameTags, ctx.derivedStats, nullptr,
                                    ctx.statsTuning);
                            LOG_INFO("Bandit hits you: {:.0f} damage{}",
                                     result.healthDamage,
                                     result.staggered ? " (staggered!)"
                                                      : "");
                        }
                    }
                }
            }
        }

        if (inCombat) {
            // combat overrode the schedule this frame
        } else if (npc.schedule.isValid()) {
            // --- Schedule-driven day (B3) ---
            updateNpcSchedule(ctx, npc, hourOfDay);
            npc.repathTimer -= dt;
            if (npc.wanderTimer > 0.0f) {
                npc.wanderTimer -= dt;
            }
            const gameplay::AiPackageForm* package = npc.activePackage;
            Vec3 anchor = transform.position;
            if (const auto* locationRef =
                    npc.activeLocation.isValid()
                        ? ctx.forms.find<world::ReferenceForm>(
                              npc.activeLocation)
                        : nullptr) {
                anchor = locationRef->position;
                anchor.y = render::terrain::height(ctx.terrainParams, anchor.x,
                                                   anchor.z);
            }
            const auto goTo = [&](const Vec3& target) {
                if (npc.pathIndex < npc.path.size() ||
                    npc.repathTimer > 0.0f) {
                    return;
                }
                const nav::PathResult found = ctx.navigator->findPath(
                    { transform.position, target, 0.8f });
                npc.path = found.success ? found.waypoints : vector<Vec3> {};
                npc.pathIndex = 0;
                npc.repathTimer = 2.0f; // budget: no repath storm
            };
            const str kind = package ? package->kind : str { "guard" };
            if (kind == "wander") {
                const f32 radius = package ? package->radius : 4.0f;
                if (npc.pathIndex >= npc.path.size() &&
                    npc.wanderTimer <= 0.0f) {
                    // Cheap per-NPC stroll target around the anchor (cosmetic
                    // randomness — not gameplay RNG, §8).
                    const u32 hash =
                        static_cast<u32>(npc.entity.id()) * 2654435761u +
                        static_cast<u32>(ctx.timeSeconds * 0.37f);
                    const f32 angle = static_cast<f32>(hash % 628) * 0.01f;
                    const f32 reach =
                        radius * (0.35f + static_cast<f32>(hash % 61) * 0.01f);
                    goTo(anchor + Vec3 { std::cos(angle) * reach, 0.0f,
                                         std::sin(angle) * reach });
                }
                if (moveNpcAlongPath(ctx, npc, dt,
                                     package ? package->speed : 1.0f)) {
                    if (npc.pathIndex >= npc.path.size() &&
                        npc.wanderTimer <= 0.0f && !npc.path.empty()) {
                        npc.path.clear();
                        npc.wanderTimer =
                            3.0f + static_cast<f32>(npc.entity.id() % 4);
                    }
                    idleDecay = 6.0f;
                }
            } else if (kind == "useFurniture" || kind == "sleep" ||
                       kind == "eat" || kind == "work") {
                goTo(anchor);
                if (moveNpcAlongPath(ctx, npc, dt,
                                     package ? package->speed : 1.0f)) {
                    // Arrived: claim a point and sit (the anim graph's
                    // State.Sitting gate does the rest).
                    if (!npc.furnitureClaimed) {
                        ctx.furnitureOccupancy.claim(npc.activeLocation, 1,
                                                     npc.entity.id());
                        npc.furnitureClaimed = true;
                    }
                    npc.sitting = true;
                }
            } else { // travel / guard / unknown: reach the spot and stand
                goTo(anchor);
                moveNpcAlongPath(ctx, npc, dt, package ? package->speed : 1.0f);
            }
        } else if (patrolPoints.size() >= 2) {
            // --- Legacy patrol fallback (chantier 1 B6) ---
            const Vec3 goal = patrolPoints[npc.target % patrolPoints.size()];
            Vec3 to = goal - transform.position;
            to.y = 0.0f;
            const f32 distance = glm::length(to);
            if (npc.pauseTimer > 0.0f) {
                npc.pauseTimer -= dt;
            } else if (distance < 0.4f) {
                npc.pauseTimer = ctx.statsTuning.npcPatrolPauseSeconds;
                npc.target =
                    (npc.target + 1) % static_cast<u32>(patrolPoints.size());
            } else {
                npc.path = { goal };
                npc.pathIndex = 0;
                moveNpcAlongPath(ctx, npc, dt, 1.0f);
            }
        }
        npc.speed -= npc.speed * (1.0f - std::exp(-idleDecay * dt)) *
                     (npc.pathIndex >= npc.path.size() ? 1.0f : 0.0f);

        // Anim: real speed feeds the param (transitions) AND the
        // referenceSpeed sync (anti-foot-sliding).
        npc.anim->setParam("speed", npc.speed);
        npc.anim->update(dt, npc.speed);
        anim::bindPose(npc.rig->skeleton, npc.pose);
        npc.anim->evaluate(npc.pose);
        anim::skinMatrices(npc.rig->skeleton, npc.pose, npc.palette);
    }
}

void NpcDirector::extract(RenderSnapshot& out) const {
    for (const auto& npcPtr : npcs_) {
        const Npc& npc = *npcPtr;
        if (npc.vertices.id == 0 || !npc.entity.is_alive()) {
            continue;
        }
        const auto& transform = npc.entity.get<world::Transform>();
        out.skinned.push_back(
            { npc.entity.id(),
              glm::translate(Mat4 { 1.0f }, transform.position) *
                  glm::mat4_cast(transform.rotation),
              npc.tint, npc.vertices, npc.indices, npc.indexCount,
              npc.palette });
    }
}

void NpcDirector::teardown(rhi::Device& device) {
    for (auto& npc : npcs_) {
        destroyNpc(device, *npc);
    }
    npcs_.clear();
    patrolPoints.clear();
    rigCache.clear();
}

} // namespace game
