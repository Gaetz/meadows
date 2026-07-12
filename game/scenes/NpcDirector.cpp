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
#include "engine/core/Rng.hpp"            // A5: NPC guard rolls (§8)
#include "engine/physics/Physics.hpp"     // phys::PhysicsWorld/CharacterBody/RayHit
#include "engine/assets/MeshData.hpp"     // render::SkinnedVertex
#include "engine/render/landscape/TerrainNoise.hpp" // terrain::height
#include "engine/rhi/Device.hpp"
#include "game/SceneSubmit.hpp"           // RenderSnapshot (U4-2b extract)
#include "game/WeaponMeshes.hpp"          // A2: the visible sword guid
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp" // attr, currentValueOf
#include "gameplay/ability/GameplayAbility.hpp" // tryActivate (P0 A3)
#include "gameplay/actors/ActorState.hpp"
#include "gameplay/actors/CharacterForms.hpp"
#include "gameplay/actors/CharacterTick.hpp"
#include "gameplay/combat/CombatAi.hpp"         // chooseCombatMove (B3)
#include "gameplay/cue/GameplayCues.hpp"        // Cue.* emissions (C2)
#include "gameplay/combat/MeleeStrike.hpp"      // the ONE strike resolution
#include "gameplay/combat/MeleeSwing.hpp"       // the blade-touch swing (A4)
#include "gameplay/combat/Projectile.hpp"       // archer NPCs (A7)
#include "game/scenes/ProjectileDirector.hpp"   // archer NPCs (A7)
#include "gameplay/ai/AiForms.hpp"
#include "gameplay/ai/ScheduleSystem.hpp"
#include "gameplay/event/EventBus.hpp"
#include "gameplay/ability/GameplayEffects.hpp" // applyEffect/removeById (D1)
#include "gameplay/interaction/Furniture.hpp"
#include "gameplay/interaction/FurnitureForms.hpp" // FurnitureForm (D1)
#include "gameplay/inventory/Inventory.hpp" // Equipment (the weapon link)
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/EquipmentStats.hpp" // weaponDamageEvent
#include "gameplay/stats/GameClock.hpp"
#include "script/Vm.hpp"               // brain scripts (BOSS-SCRIPTING.md)
#include "world/ai/Perception.hpp"     // B2: vision cone + aware states
#include "world/ai/TerrainNavigator.hpp"
#include "world/scene/AnimBridge.hpp"     // resolveActorVisual, buildAnimGraph
#include "world/scene/Components.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace game {

namespace {

// U4-7: the stat-space -> world mapping and the NPC gait now come from
// StatsTuningForm (§5 moddable) — the same scale the player uses, no more
// hand-mirrored copy.

// P0 A2/A3 [cpp-tuning] — the sword grip correction for the UAL hand_r
// joint (dev feel pass 2026-07-11). Hand-local: fingers run along +Y,
// the thumb sits on +Z, X pierces the palm. Identity put the blade in
// the FOREARM'S prolongation (along the fingers); +90 degrees about X —
// "the axis through the hand" — stands it up out of the fist on the
// thumb side, where a gripped handle actually exits. Applied to the
// DRAWN sword (extract) and the HIT segment (update) alike: the blade
// that hits stays the blade you see.
const Mat4 kSwordGrip = glm::rotate(
    Mat4 { 1.0f }, glm::radians(90.0f), Vec3 { 1.0f, 0.0f, 0.0f });

// A5+ [cpp-tuning] — the raised-guard grip (dev design: the hand turns
// a little INWARD so the blade lies oblique across the front). An extra
// roll about the fist axis on top of kSwordGrip; drawn while
// npc.blocking (the hit test never runs during a guard).
const Mat4 kSwordGuardGrip =
    kSwordGrip * glm::rotate(Mat4 { 1.0f }, glm::radians(-40.0f),
                             Vec3 { 0.0f, 0.0f, 1.0f });

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
            npc->courage = actor.courage; // B3: flees below (1 - courage)
            // Brain script: Lua decides this actor's combat moves
            // (docs/BOSS-SCRIPTING.md); keyed by the FORM — every
            // instance shares one compiled decide.
            npc->brainScript = actor.brainScript;
            npc->brainKey = actor.id;
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
                    if (raw->sitting && tag == raw->sitGate) {
                        return true; // D1: the claimed point's animTag
                    }
                    if (tag == "State.Dead") {
                        return raw->dead;
                    }
                    if (tag == "State.Attacking") { // P0 A3: swing gate
                        return raw->attacking;
                    }
                    return false;
                });
            // Chantier P0 C4a: the sink FINALLY gets a runtime consumer —
            // events buffer on the Npc (uptr = stable address) and drain
            // onto the EventBus in update(), where the context lives.
            npc->anim->setEventSink(
                [raw = npc.get()](std::string_view name) {
                    raw->pendingAnimEvents.emplace_back(name);
                });
            // A2: the sword hand (UAL rig: "hand_r"); -1 = no weapon shown.
            npc->handJoint = rig->skeleton.findJoint("hand_r");
            npc->tint = visual->tint;
            // The pose is normally written by update(); a paused sim (boot
            // = Spectator) extracts BEFORE any update, and the A2 weapon
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
        // B2: every built NPC perceives (a reload keeps a saved state
        // through the reflected component; a fresh one is Calm). ADDING
        // the component is a table move — it must run out here with the
        // loadouts, never inside the locked .each above (flecs
        // LOCKED_STORAGE: fatal in Debug, silent corruption in Release).
        if (!entity.has<world::Perception>()) {
            entity.set<world::Perception>({});
        }
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
        releaseFurniture(ctx, npc); // D1: occupancy + effect + gate
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

// B3+: is a pathless steering step about to walk into a wall? Direct
// moves (strafe, cornered flee) bypass the navigator, so probe the way
// with a chest-height ray — buildings are static colliders, the ray
// sees them (actors are NOT in the broadphase; other NPCs don't block).
static bool steerBlocked(const NpcContext& ctx, const Vec3& from,
                         const Vec3& direction) {
    if (!ctx.physics) {
        return false;
    }
    const Vec3 chest = from + Vec3 { 0.0f, 0.9f, 0.0f };
    const phys::RayHit hit = ctx.physics->rayCast(chest, direction, 0.9f);
    return hit.hit;
}

void NpcDirector::moveNpcDirect(const NpcContext& ctx, Npc& npc, f32 dt,
                                const Vec3& direction, f32 speedScale,
                                f32 faceYaw) {
    auto& transform = npc.entity.get_mut<world::Transform>();
    const auto& sys = npc.entity.get<gameplay::AbilitySystem>();
    const f32 walkSpeed =
        gameplay::currentValueOf(sys, gameplay::attr("movementSpeed")) *
        ctx.statsTuning.movementSpeedScale3D * ctx.statsTuning.npcWalkFactor *
        speedScale;
    transform.position += direction * walkSpeed * dt;
    transform.position.y = render::terrain::height(
        ctx.terrainParams, transform.position.x, transform.position.z);
    npc.yaw = faceYaw;
    transform.rotation = glm::angleAxis(npc.yaw, Vec3 { 0.0f, 1.0f, 0.0f });
    npc.speed += (walkSpeed - npc.speed) * (1.0f - std::exp(-10.0f * dt));
    npc.steered = true; // pathless but MOVING: skip the idle speed decay
}

void NpcDirector::releaseFurniture(const NpcContext& ctx, Npc& npc) {
    if (npc.furnitureClaimed) {
        ctx.furnitureOccupancy.release(npc.entity.id());
        npc.furnitureClaimed = false;
    }
    if (npc.furnitureEffectId != 0 &&
        npc.entity.has<gameplay::AbilitySystem>()) {
        // D1: standing up ends the furniture's effect (rest regen...).
        gameplay::removeEffectById(
            npc.entity.get_mut<gameplay::AbilitySystem>(),
            npc.furnitureEffectId, ctx.gameTags);
        npc.furnitureEffectId = 0;
    }
    npc.sitting = false;
}

void NpcDirector::callForHelp(const NpcContext& ctx, const Npc& caller,
                              const Vec3& targetPos) {
    // The shout on the bus first: quests/scripts/mods can listen.
    ctx.eventBus.dispatch({ gameplay::eventKind("CallForHelp"),
                            caller.entity, ecs::Entity {},
                            caller.factionTag });
    if (!caller.factionTag.isValid()) {
        return; // no faction, no friends
    }
    const Vec3 callerPos = caller.entity.get<world::Transform>().position;
    const f32 radius = ctx.statsTuning.helpCallRadius;
    for (auto& allyPtr : npcs_) {
        Npc& ally = *allyPtr;
        if (&ally == &caller || ally.dead || !ally.entity.is_alive() ||
            ally.factionTag != caller.factionTag ||
            !ally.entity.has<world::Perception>()) {
            continue;
        }
        const Vec3 gap =
            ally.entity.get<world::Transform>().position - callerPos;
        if (glm::dot(gap, gap) > radius * radius) {
            continue;
        }
        world::alertTo(ally.entity.get_mut<world::Perception>(), targetPos);
        ally.sitting = false; // an alerted ally stands up
    }
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
    // Sneak: a crouched player is HALF the target — sight range, the
    // LOS aim point and the blade capsule all read this one bool.
    bool playerSneaking = false;
    if (ctx.playerEntity.is_alive()) {
        if (const auto sneakTag = ctx.gameTags.find("State.Sneaking")) {
            playerSneaking =
                ctx.playerEntity.get<gameplay::AbilitySystem>().tags.has(
                    *sneakTag);
        }
    }
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
            if (ctx.cues) { // C2: the fall's LOOK (dust puff by default)
                ctx.cues->emit({ "Cue.Death",
                                 transform.position +
                                     Vec3 { 0.0f, 0.6f, 0.0f },
                                 0.0f });
            }
        }
        // (The corpse is lootable — its Inventory was rolled from the
        // LoadoutEntryForm children at build, chantier 4 B5.)
        if (npc.dead) {
            // The death transition (anim graph, State.Dead gate) plays; the
            // body stays. Despawn: a later slice.
            releaseFurniture(ctx, npc); // D1: a corpse frees its seat
            npc.attacking = false;   // a death mid-swing cancels it
            npc.weaponDrawn = false; // the club drops with him (visually)
            npc.path.clear();
            npc.speed = 0.0f;
            npc.anim->setParam("speed", 0.0f);
            npc.anim->update(dt, 0.0f);
            anim::bindPose(npc.rig->skeleton, npc.pose);
            npc.anim->evaluate(npc.pose);
            anim::skinMatrices(npc.rig->skeleton, npc.pose, npc.palette);
            npc.pendingAnimEvents.clear(); // dead men fire no events
            continue;
        }

        // The NPC fights with the weapon it EQUIPPED (the loadout equips
        // the first weapon it rolled — the inventory link): stats, reach,
        // timings AND the drawn model all come from this one form.
        // banditWeapon stays the armed-and-equipmentless fallback.
        const data::WeaponForm* npcWeapon = ctx.banditWeapon;
        if (npc.entity.has<gameplay::Equipment>()) {
            const core::Guid equipped =
                npc.entity.get<gameplay::Equipment>().weapon;
            if (equipped.isValid()) {
                if (const auto* form =
                        ctx.forms.find<data::WeaponForm>(equipped)) {
                    npcWeapon = form;
                }
            }
        }
        npc.weaponModel = npcWeapon && npcWeapon->model.isValid()
                              ? npcWeapon->model
                              : core::Guid {};

        // B5→B2: hostile actors perceive the player — vision cone + LOS
        // feed the Perception state machine; Alert hunts, Searching walks
        // to the last known position and gives up on a timeout. D2: a
        // guard turns hostile while the player carries a bounty
        // (tag-based — the relations table stays a later pass).
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
            auto& perception = npc.entity.get_mut<world::Perception>();
            // Vision verdict: cone from the NPC's facing, then the LOS
            // raycast (world geometry only — actors are out of the
            // broadphase anyway). A SNEAKING target is spotted at half
            // range (the sneak skill will drive the factor later), and
            // the LOS aims at the CROUCHED chest — a low wall now hides.
            world::Perception sight = perception;
            if (playerSneaking) {
                sight.viewDistance *= ctx.statsTuning.sneakDetectionFactor;
            }
            const Vec3 facing { std::sin(npc.yaw), 0.0f,
                                std::cos(npc.yaw) };
            bool canSee = world::inViewCone(sight, transform.position,
                                            facing, playerPos);
            if (canSee && ctx.physics) {
                const Vec3 eye =
                    transform.position + Vec3 { 0.0f, 1.5f, 0.0f };
                const Vec3 target =
                    playerPos +
                    Vec3 { 0.0f, playerSneaking ? 0.6f : 1.2f, 0.0f };
                const Vec3 dir = glm::normalize(target - eye);
                const f32 sight = glm::length(target - eye);
                const phys::RayHit hit = ctx.physics->rayCast(eye, dir,
                                                              sight);
                canSee = !(hit.hit && hit.distance < sight - 0.6f);
            }
            const world::AwareState wasAware = world::awareState(perception);
            world::updatePerception(perception, canSee, playerPos, dt);
            const world::AwareState aware = world::awareState(perception);
            // B3: entering Alert shouts — a bus event for listeners, and
            // same-faction allies in earshot join the hunt.
            if (aware == world::AwareState::Alert &&
                wasAware != world::AwareState::Alert) {
                callForHelp(ctx, npc, perception.lastKnownPos);
            }
            // STATS.md §4: a staggered actor can't act, parry or dodge
            // and moves at a crawl — the bandit just STANDS there,
            // reeling (the riposte window the parry earns).
            bool npcStaggered = false;
            if (const auto staggerTag =
                    ctx.gameTags.find("State.Staggered")) {
                npcStaggered = npcSys.tags.has(*staggerTag);
            }
            if (npcStaggered) {
                inCombat = true; // reeling still overrides the schedule
                releaseFurniture(ctx, npc); // D1: knocked off the seat
                npc.blocking = false;
                npc.path.clear();
                npc.attackCooldown -= dt;
            } else if (aware == world::AwareState::Alert ||
                       aware == world::AwareState::Searching) {
                inCombat = true;
                releaseFurniture(ctx, npc); // D1: combat stands him up
                npc.attackCooldown -= dt;
                npc.repathTimer -= dt;
                // A7+: an archer's quiver is REAL — the loadout rolls his
                // arrows and each shot consumes one. Dry (or no Inventory
                // at all) = no ranged option: the reach collapses to melee
                // so the combat brain closes in and clubs with the bow.
                bool quiverDry = false;
                if (npcWeapon && npcWeapon->projectileSpeed > 0.0f &&
                    npcWeapon->ammo.isValid()) {
                    quiverDry =
                        !npc.entity.has<gameplay::Inventory>() ||
                        gameplay::itemCount(
                            npc.entity.get<gameplay::Inventory>(),
                            npcWeapon->ammo) <= 0;
                }
                // P0 A6: the engagement distances come from the WEAPON
                // (§5 moddable) — a spear-armed NPC stands off further
                // than a knife mugger, no code change.
                const f32 reach =
                    npcWeapon && !quiverDry ? npcWeapon->reach : 2.1f;
                const f32 attackRange = glm::max(reach - 0.3f, 0.8f);
                // P0 A3: a swing in flight roots the NPC (the clip plays
                // out; the blade does the hitting below).
                const bool swinging =
                    npc.entity.get<gameplay::MeleeSwing>().phase !=
                    gameplay::SwingPhase::Idle;
                Vec3 toPlayer = playerPos - transform.position;
                toPlayer.y = 0.0f;
                const f32 playerDistance = glm::length(toPlayer);
                // B3: the whole behavior choice is ONE sim-pure function
                // (gameplay/combat/CombatAi) — this block only executes
                // the move it returns.
                const f32 maxHealth = glm::max(
                    gameplay::currentValueOf(npcSys,
                                             gameplay::attr("maxHealth")),
                    1.0f);
                const gameplay::CombatSituation situation {
                    playerDistance,
                    attackRange,
                    reach + 1.0f,
                    canSee,
                    swinging,
                    npc.attackCooldown,
                    gameplay::currentValueOf(npcSys,
                                             gameplay::attr("health")) /
                        maxHealth,
                    npc.courage
                };
                // The C++ brain by default; a brain SCRIPT (Lua, decision
                // tick — never per frame) overrides the move when it
                // returns a valid name. Errors fall back silently
                // (callBrain logs once and drops the script).
                gameplay::CombatMove move =
                    gameplay::chooseCombatMove(situation);
                if (!npc.brainScript.empty() && ctx.vm) {
                    npc.brainTimer -= dt;
                    if (npc.brainTimer <= 0.0f) {
                        npc.brainTimer = 0.25f; // [cpp-tuning] ~4 Hz
                        script::ScriptContext sctx;
                        sctx.entity = npc.entity;
                        sctx.attributes =
                            &npc.entity.get_mut<gameplay::AttributeSet>();
                        sctx.abilitySystem =
                            &npc.entity.get_mut<gameplay::AbilitySystem>();
                        sctx.tags = &ctx.gameTags;
                        sctx.forms = &ctx.forms;
                        npc.brainMove = gameplay::parseCombatMove(
                            ctx.vm->callBrain(
                                npc.brainKey, npc.brainScript, sctx,
                                situation,
                                aware == world::AwareState::Alert
                                    ? "alert"
                                    : "searching"));
                    }
                    if (npc.brainMove) {
                        move = *npc.brainMove;
                    }
                }
                switch (move) {
                case gameplay::CombatMove::Strike: {
                    npc.path.clear();
                    // Face the player and swing.
                    npc.yaw = std::atan2(toPlayer.x, toPlayer.z);
                    transform.rotation = glm::angleAxis(
                        npc.yaw, Vec3 { 0.0f, 1.0f, 0.0f });
                    // P0 A3: instant damage became an ability-gated
                    // MeleeSwing — the Sword_Attack clip carries the
                    // hand, and the blade must TOUCH (below).
                    if (!swinging && npc.attackCooldown <= 0.0f &&
                        npcWeapon && ctx.playerEntity.is_alive() &&
                        !ctx.godMode) {
                        auto& set =
                            npc.entity.get_mut<gameplay::AttributeSet>();
                        auto& system =
                            npc.entity.get_mut<gameplay::AbilitySystem>();
                        const bool activated =
                            !ctx.attackAbility ||
                            gameplay::tryActivate(*ctx.attackAbility, set,
                                                  system, set, system,
                                                  { ctx.forms,
                                                    ctx.gameTags });
                        if (activated &&
                            npcWeapon->projectileSpeed > 0.0f &&
                            !quiverDry && ctx.projectiles) {
                            // A7: an ARCHER — loose from the chest at
                            // the player's chest, with a hair of spread
                            // (deterministic combat RNG, §8).
                            gameplay::Projectile arrow;
                            arrow.position = transform.position +
                                             Vec3 { 0.0f, 1.4f, 0.0f };
                            Vec3 aim =
                                (playerPos + Vec3 { 0.0f, 1.0f, 0.0f }) -
                                arrow.position;
                            aim = glm::normalize(aim);
                            aim.x += (static_cast<f32>(
                                          ctx.combatRng.unit()) -
                                      0.5f) *
                                     0.06f;
                            aim.z += (static_cast<f32>(
                                          ctx.combatRng.unit()) -
                                      0.5f) *
                                     0.06f;
                            arrow.velocity = glm::normalize(aim) *
                                             npcWeapon->projectileSpeed;
                            arrow.shooter = npc.entity.id();
                            arrow.payload = gameplay::weaponDamageEvent(
                                *npcWeapon, npcSys);
                            // A7+: the shot spends an arrow from HIS
                            // inventory (quiverDry gated above); planted
                            // arrows stay loot for whoever walks by.
                            arrow.ammoItem = npcWeapon->ammo;
                            if (npcWeapon->ammo.isValid() &&
                                npc.entity.has<gameplay::Inventory>()) {
                                gameplay::removeItem(
                                    npc.entity
                                        .get_mut<gameplay::Inventory>(),
                                    npcWeapon->ammo);
                            }
                            ctx.projectiles->spawn(arrow);
                            npc.attackCooldown = 2.2f; // [cpp-tuning]
                            npc.blocking = false;
                        } else if (activated) {
                            gameplay::startSwing(
                                npc.entity
                                    .get_mut<gameplay::MeleeSwing>());
                            // [cpp-tuning] pause between swings.
                            npc.attackCooldown = 1.6f;
                            npc.blocking = false; // guard drops to strike
                        }
                    }
                    break;
                }
                case gameplay::CombatMove::Strafe: {
                    npc.path.clear();
                    // Orbit the target: a tangent direction whose side is
                    // picked by entity-id parity (stable per NPC — two
                    // bandits circle opposite ways) plus a radial drift
                    // holding the middle of the weapon band.
                    const Vec3 radial =
                        toPlayer / glm::max(playerDistance, 1e-3f);
                    const f32 side =
                        (npc.entity.id() & 1u) != 0u ? 1.0f : -1.0f;
                    const f32 band = (attackRange + reach + 1.0f) * 0.5f;
                    Vec3 dir =
                        glm::cross(Vec3 { 0.0f, 1.0f, 0.0f }, radial) *
                            side +
                        radial * glm::clamp(playerDistance - band, -1.0f,
                                            1.0f) *
                            0.6f;
                    dir.y = 0.0f;
                    const f32 len = glm::length(dir);
                    // Wall guard: direct steering skips the navigator,
                    // so probe the step — blocked = hold ground (still
                    // facing the player).
                    if (len > 1e-4f &&
                        !steerBlocked(ctx, transform.position,
                                      dir / len)) {
                        moveNpcDirect(ctx, npc, dt, dir / len, 1.0f,
                                      std::atan2(toPlayer.x, toPlayer.z));
                    }
                    break;
                }
                case gameplay::CombatMove::Flee: {
                    // Flee through the NAVIGATOR (dev report 2026-07-12:
                    // direct steering ran straight through buildings) —
                    // path to a spot away from the player, repathed as
                    // the flight goes on; obstacles are its job.
                    const Vec3 away =
                        playerDistance > 1e-3f
                            ? -toPlayer / playerDistance
                            : Vec3 { std::sin(npc.yaw), 0.0f,
                                     std::cos(npc.yaw) };
                    // A broken fighter RUNS (dev feel pass): cancel the
                    // NPC walk factor so he flees at full jog speed —
                    // solidly inside the run anim's threshold.
                    const f32 runScale =
                        1.0f /
                        glm::max(ctx.statsTuning.npcWalkFactor, 0.05f);
                    if (npc.pathIndex >= npc.path.size() ||
                        npc.repathTimer <= 0.0f) {
                        Vec3 spot = transform.position + away * 12.0f;
                        spot.y = render::terrain::height(ctx.terrainParams,
                                                         spot.x, spot.z);
                        const nav::PathResult found =
                            ctx.navigator->findPath(
                                { transform.position, spot, 2.0f });
                        npc.path = found.success ? found.waypoints
                                                 : vector<Vec3> {};
                        npc.pathIndex = 0;
                        npc.repathTimer = 0.8f;
                    }
                    if (npc.pathIndex < npc.path.size()) {
                        moveNpcAlongPath(ctx, npc, dt, runScale);
                    } else if (!steerBlocked(ctx, transform.position,
                                             away)) {
                        // No path (cornered): raw retreat, wall-guarded.
                        moveNpcDirect(ctx, npc, dt, away, runScale,
                                      std::atan2(away.x, away.z));
                    }
                    break;
                }
                case gameplay::CombatMove::Approach: {
                    // Hunt the player while seen; investigate the last
                    // known position otherwise (B2).
                    const Vec3 goal =
                        canSee ? playerPos : perception.lastKnownPos;
                    Vec3 toGoal = goal - transform.position;
                    toGoal.y = 0.0f;
                    const f32 goalDistance = glm::length(toGoal);
                    if (!swinging && goalDistance > 0.6f) {
                        if (npc.repathTimer <= 0.0f) {
                            const nav::PathResult found =
                                ctx.navigator->findPath(
                                    { transform.position, goal, 1.2f });
                            npc.path = found.success ? found.waypoints
                                                     : vector<Vec3> {};
                            npc.pathIndex = 0;
                            npc.repathTimer = 1.0f;
                        }
                        // Alert hurries; a search walks (may be nothing).
                        moveNpcAlongPath(ctx, npc, dt,
                                         aware == world::AwareState::Alert
                                             ? 1.8f
                                             : 1.0f);
                    } else if (!canSee) {
                        // Arrived at the last known spot and no one is
                        // here: stand — the patience runs out on its own.
                        npc.path.clear();
                    }
                    break;
                }
                }
            }
        }

        if (!inCombat) {
            npc.blocking = false; // A5: the fight is over, lower the guard
        }
        // Drawn while fighting, back on the belt when it calms down —
        // extract reads this (the sim decides, the renderer shows).
        npc.weaponDrawn = inCombat;
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
                const bool pathDone = moveNpcAlongPath(
                    ctx, npc, dt, package ? package->speed : 1.0f);
                Vec3 toAnchor = anchor - transform.position;
                toAnchor.y = 0.0f;
                const f32 anchorDist = glm::length(toAnchor);
                // The grid path often can't END on the furniture (the
                // prop is its own nav obstacle): close the last meters
                // DIRECTLY, collider-guarded — flush against the crate
                // counts as arrived (dev report 2026-07-12).
                bool arrived = pathDone && anchorDist <= 0.9f;
                if (pathDone && !arrived) {
                    const Vec3 dir = toAnchor / glm::max(anchorDist, 1e-4f);
                    if (steerBlocked(ctx, transform.position, dir)) {
                        arrived = anchorDist <= 1.6f; // against the prop
                    } else {
                        moveNpcDirect(ctx, npc, dt, dir, 1.0f,
                                      std::atan2(dir.x, dir.z));
                    }
                }
                if (arrived) {
                    // Claim a point and sit. D1: the claimed POINT's
                    // animTag drives the anim gate ("State." + tag), and
                    // the furniture's GAS effect (rest regen, warmth...)
                    // applies for as long as the seat is held.
                    if (!npc.furnitureClaimed) {
                        npc.sitGate = "State.Sitting";
                        const gameplay::FurnitureForm* furniture = nullptr;
                        if (const auto* ref =
                                ctx.forms.find<world::ReferenceForm>(
                                    npc.activeLocation)) {
                            furniture =
                                ctx.forms.find<gameplay::FurnitureForm>(
                                    ref->baseForm);
                        }
                        u32 pointCount = 0;
                        if (furniture) {
                            data::childrenOf<gameplay::FurniturePointForm>(
                                ctx.forms, furniture->id,
                                [&](const gameplay::FurniturePointForm&) {
                                    ++pointCount;
                                });
                        }
                        const auto point = ctx.furnitureOccupancy.claim(
                            npc.activeLocation, glm::max(pointCount, 1u),
                            npc.entity.id());
                        npc.furnitureClaimed = true;
                        if (furniture) {
                            u32 index = 0;
                            Vec3 seatOffset { 0.0f };
                            data::childrenOf<gameplay::FurniturePointForm>(
                                ctx.forms, furniture->id,
                                [&](const gameplay::FurniturePointForm& p) {
                                    if (point && index == *point) {
                                        npc.sitGate = "State." + p.animTag;
                                        seatOffset = p.offset;
                                    }
                                    ++index;
                                });
                            // Sit ON the point (the crate top), not
                            // beside it — release paths re-ground him.
                            transform.position = anchor + seatOffset;
                            if (furniture->effect.isValid()) {
                                if (const auto* effect =
                                        ctx.forms
                                            .find<gameplay::EffectForm>(
                                                furniture->effect)) {
                                    gameplay::applyEffect(
                                        npc.entity.get_mut<
                                            gameplay::AttributeSet>(),
                                        npc.entity.get_mut<
                                            gameplay::AbilitySystem>(),
                                        *effect, ctx.gameTags, nullptr,
                                        &npc.furnitureEffectId);
                                }
                            }
                        }
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
        // Standing = no path AND no direct steering this frame (strafe
        // and flee move pathless — their run must reach the anim).
        npc.speed -= npc.speed * (1.0f - std::exp(-idleDecay * dt)) *
                     (npc.pathIndex >= npc.path.size() && !npc.steered
                          ? 1.0f
                          : 0.0f);
        npc.steered = false; // consumed — re-armed by the next steer

        // Anim: real speed feeds the param (transitions) AND the
        // referenceSpeed sync (anti-foot-sliding).
        npc.anim->setParam("speed", npc.speed);
        npc.anim->update(dt, npc.speed);
        anim::bindPose(npc.rig->skeleton, npc.pose);
        npc.anim->evaluate(npc.pose);
        anim::skinMatrices(npc.rig->skeleton, npc.pose, npc.palette);

        // P0 A3/A4: the swing machine + the blade-touch hit (the SAME
        // MeleeSwing code path as the player). The clip's authored
        // HitOpen/HitClose events override the data windows; the hit
        // segment is the VISIBLE blade — world x hand joint x +Y, exactly
        // what extract() draws — against the player capsule.
        auto& swing = npc.entity.get_mut<gameplay::MeleeSwing>();
        if (swing.phase != gameplay::SwingPhase::Idle && npcWeapon) {
            for (const str& name : npc.pendingAnimEvents) {
                gameplay::onSwingAnimEvent(swing, name);
            }
            const gameplay::SwingTiming timing {
                npcWeapon->swingWindup, npcWeapon->swingActive,
                npcWeapon->swingRecovery
            };
            gameplay::updateSwing(swing, dt, timing);
            if (swing.phase == gameplay::SwingPhase::Idle) {
                // A5: the guard window between swings — ONE roll per
                // window, on the seeded engine RNG (§8).
                npc.blocking = ctx.combatRng.chance(
                    static_cast<f64>(ctx.statsTuning.npcBlockChance));
            }
            if (swing.phase == gameplay::SwingPhase::Active &&
                npc.handJoint >= 0 && ctx.playMode && ctx.player &&
                ctx.playerEntity.is_alive() && !ctx.godMode) {
                const Mat4 world =
                    glm::translate(Mat4 { 1.0f }, transform.position) *
                    glm::mat4_cast(transform.rotation);
                anim::modelMatrices(npc.rig->skeleton, npc.pose,
                                    jointScratch);
                const Mat4 hand =
                    world *
                    jointScratch[static_cast<size_t>(npc.handJoint)] *
                    kSwordGrip;
                const Vec3 grip { hand[3] };
                const Vec3 bladeDir = glm::normalize(Vec3 { hand[1] });
                const Vec3 tip =
                    grip + bladeDir * (npcWeapon->bladeLength *
                                       npcWeapon->hitTolerance);
                const Vec3 feet = ctx.player->position();
                // Dodge i-frames: State.Dodging means the blade passes
                // through — and does NOT register, so the same Active
                // window can still connect once the i-frames expire.
                bool dodging = false;
                if (const auto dodgeTag =
                        ctx.gameTags.find("State.Dodging")) {
                    dodging = ctx.playerEntity
                                  .get<gameplay::AbilitySystem>()
                                  .tags.has(*dodgeTag);
                }
                // A crouched player is half the target (sneak rule).
                if (!dodging &&
                    gameplay::segmentHitsActor(grip, tip, feet,
                                               playerSneaking) &&
                    gameplay::registerStrike(swing,
                                             ctx.playerEntity.id())) {
                    // The exchange rules (crit window, guard cone,
                    // perfect parry, events, cues) live in ONE place,
                    // resolveMeleeStrike, shared with the player side.
                    gameplay::StatBlock defender {
                        ctx.playerEntity.get_mut<gameplay::CoreAttributes>(),
                        ctx.playerEntity.get_mut<gameplay::AttributeSet>(),
                        ctx.playerEntity.get_mut<gameplay::AbilitySystem>(),
                        ctx.playerEntity.get_mut<gameplay::CombatState>()
                    };
                    gameplay::StatBlock attacker {
                        npc.entity.get_mut<gameplay::CoreAttributes>(),
                        npc.entity.get_mut<gameplay::AttributeSet>(),
                        npc.entity.get_mut<gameplay::AbilitySystem>(),
                        npc.entity.get_mut<gameplay::CombatState>()
                    };
                    const auto& playerT =
                        ctx.playerEntity.get<world::Transform>();
                    const gameplay::StrikeGeometry geo {
                        transform.position, playerT.position,
                        playerT.rotation * Vec3 { 0.0f, 0.0f, 1.0f },
                        ctx.playerEntity.get<gameplay::MeleeSwing>()
                            .guardSeconds,
                        feet + Vec3 { 0.0f, 1.2f, 0.0f }
                    };
                    const gameplay::StrikeContext strikeCtx {
                        ctx.gameTags, ctx.derivedStats, ctx.statsTuning,
                        &ctx.eventBus, ctx.cues
                    };
                    const gameplay::StrikeOutcome outcome =
                        gameplay::resolveMeleeStrike(
                            attacker, defender, npc.entity,
                            ctx.playerEntity,
                            gameplay::weaponDamageEvent(*npcWeapon,
                                                        attacker.system),
                            geo, strikeCtx);
                    if (outcome.guard.perfect) {
                        LOG_INFO("PERFECT PARRY — bandit poise -{}{}",
                                 ctx.statsTuning.perfectParryPosture,
                                 outcome.riposte.staggered
                                     ? " (STAGGERED!)" : "");
                    } else {
                        LOG_INFO("Bandit's blade lands: {:.0f} damage{}{}",
                                 outcome.damage.healthDamage,
                                 outcome.guard.caught ? " (blocked)" : "",
                                 outcome.damage.staggered
                                     ? " (staggered!)" : "");
                    }
                }
            }
        }
        npc.attacking = swing.phase != gameplay::SwingPhase::Idle;
        // A5+: the guard clock — a player hit landing while this guard
        // is FRESH gets perfect-parried (applyHit reads guardSeconds).
        gameplay::tickGuard(swing, npc.blocking, dt);
        // A5: mirror the guard onto the §6 tag vocabulary — the damage
        // paths (player applyHit, future sources) read State.Blocking,
        // never the Npc struct.
        if (const auto blockTag = ctx.gameTags.find("State.Blocking")) {
            auto& mutableSys = npc.entity.get_mut<gameplay::AbilitySystem>();
            const bool tagged = mutableSys.tags.has(*blockTag);
            if (npc.blocking && !tagged) {
                mutableSys.tags.add(*blockTag, ctx.gameTags);
            } else if (!npc.blocking && tagged) {
                mutableSys.tags.remove(*blockTag, ctx.gameTags);
            }
        }

        // Chantier P0 C4a: drain the anim events the sink buffered onto
        // the bus — ONE kind ("AnimEvent"), the clip's name in `name`;
        // hit windows (A4, routed above) and footsteps (C4b) filter on it.
        for (str& name : npc.pendingAnimEvents) {
            gameplay::Event event;
            event.kind = gameplay::eventKind("AnimEvent");
            event.source = npc.entity;
            event.name = std::move(name);
            ctx.eventBus.dispatch(event);
        }
        npc.pendingAnimEvents.clear();
    }
}

void NpcDirector::extract(RenderSnapshot& out) const {
    for (const auto& npcPtr : npcs_) {
        const Npc& npc = *npcPtr;
        if (npc.vertices.id == 0 || !npc.entity.is_alive()) {
            continue;
        }
        const auto& transform = npc.entity.get<world::Transform>();
        const Mat4 world =
            glm::translate(Mat4 { 1.0f }, transform.position) *
            glm::mat4_cast(transform.rotation);
        out.skinned.push_back({ npc.entity.id(), world, npc.tint,
                                npc.vertices, npc.indices, npc.indexCount,
                                npc.palette });
        // Chantier P0 A2: a fighting NPC carries its EQUIPPED weapon in
        // hand_r — the very blade the hit test follows (blade-touch
        // combat). Drawn only while the sim says so (weaponDrawn);
        // kSwordGrip stands it up out of the fist; a raised guard turns
        // it oblique across the front (A5+).
        if (npc.weaponDrawn && !npc.dead && npc.handJoint >= 0) {
            anim::modelMatrices(npc.rig->skeleton, npc.pose, jointScratch);
            out.meshes.push_back(
                { npc.weaponModel.isValid() ? npc.weaponModel
                                            : swordMeshGuid(),
                  core::Guid {},
                  world *
                      jointScratch[static_cast<size_t>(npc.handJoint)] *
                      (npc.blocking ? kSwordGuardGrip : kSwordGrip) });
        }
    }
}

void NpcDirector::onNoise(const Vec3& position, f32 loudness) {
    // B2 hearing: every living perceiver within ITS hearing radius turns
    // toward the noise (Calm -> Suspicious; searches re-aim; Alert
    // ignores it). Dispatchers: player footsteps and combat cues (C4b).
    for (auto& npcPtr : npcs_) {
        Npc& npc = *npcPtr;
        if (npc.dead || !npc.entity.is_alive() ||
            !npc.entity.has<world::Perception>()) {
            continue;
        }
        world::hearNoise(npc.entity.get_mut<world::Perception>(),
                         npc.entity.get<world::Transform>().position,
                         position, loudness);
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
