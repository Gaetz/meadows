#include "game/scenes/StreamingController.hpp"

#include <algorithm>

#include "data/forms/FormDatabase.hpp"
#include "engine/reflect/Reflect.hpp"
#include "engine/render/landscape/TerrainNoise.hpp" // terrain::height, TerrainParams
#include "game/MeshCache.hpp"
#include "world/ai/TerrainNavigator.hpp"
#include "world/worldspace/WorldForms.hpp" // world::CellForm, ReferenceForm

namespace game {

void StreamingController::init(ecs::World& world) {
    colliderQuery = world.handle()
                        .query<const world::Transform, const world::RefId,
                               const world::MeshRender>();
}

void StreamingController::reset(phys::PhysicsWorld* physics) {
    if (physics) {
        for (const auto& [entity, body] : staticColliders) {
            physics->removeBody(body);
        }
    }
    staticColliders.clear();
    nonCollidable.clear();
}

void StreamingController::updateStaticColliders(const StreamingContext& ctx) {
    if (!ctx.physics || !ctx.meshCache) {
        return;
    }
    for (auto it = staticColliders.begin(); it != staticColliders.end();) {
        if (!ctx.world.handle().is_alive(
                static_cast<flecs::entity_t>(it->first))) {
            ctx.physics->removeBody(it->second);
            it = staticColliders.erase(it);
        } else {
            ++it;
        }
    }
    // Cook budget: a Jolt MeshShape cook is main-thread and expensive —
    // a cell's worth of kit meshes turning resident in one frame used to
    // cost 100+ ms (the frame-probe smoking gun). Two per frame in
    // normal play, UNCAPPED while the travel fade holds the screen black
    // (the fade exists to hide exactly this — and a starved budget once
    // dropped the player through a not-yet-solid floor). NEAREST FIRST,
    // so the ground underfoot is always the first body to exist.
    u32 cookBudget = ctx.fastCook ? 4096 : 2;
    struct CookCandidate {
        f32 distSq;
        u64 id;
        const MeshCache::CpuMesh* cpu;
        Vec3 position;
        Quat rotation;
        Vec3 scale;
    };
    vector<CookCandidate> cooks;
    colliderQuery.each(
        [&](flecs::entity e, const world::Transform& transform,
            const world::RefId& ref, const world::MeshRender& mesh) {
            const u64 id = e.id();
            if (staticColliders.contains(id) || nonCollidable.contains(id)) {
                return;
            }
            // `collides` read through reflection: any base form declaring
            // it opts in (StaticForm today, DoorForm...). The negative
            // verdict is cached — reflection must not run per frame.
            const data::Form* base = ctx.forms.get(ref.base);
            const reflect::TypeInfo* type = ctx.forms.typeOf(ref.base);
            if (!base || !type) {
                nonCollidable.insert(id);
                return;
            }
            const reflect::FieldInfo* field = type->findField("collides");
            if (!field || field->kind != reflect::FieldKind::Bool ||
                !std::get<bool>(field->get(base))) {
                nonCollidable.insert(id);
                return;
            }
            const MeshCache::CpuMesh* cpu = ctx.meshCache->cpuMesh(mesh.model);
            if (!cpu) {
                return; // still streaming — retried next frame
            }
            const Vec3 d = transform.position - ctx.focus;
            cooks.push_back({ glm::dot(d, d), id, cpu, transform.position,
                              transform.rotation, transform.scale });
        });
    std::sort(cooks.begin(), cooks.end(),
              [](const CookCandidate& a, const CookCandidate& b) {
                  return a.distSq < b.distSq;
              });
    for (const CookCandidate& cook : cooks) {
        if (cookBudget == 0) {
            break;
        }
        const phys::BodyId body = ctx.physics->addStaticMesh(
            cook.cpu->positions.data(),
            static_cast<u32>(cook.cpu->positions.size()),
            cook.cpu->indices.data(),
            static_cast<u32>(cook.cpu->indices.size()), cook.position,
            cook.rotation, cook.scale);
        if (body != 0) {
            staticColliders.emplace(cook.id, body);
            --cookBudget;
        }
    }
}

// Idempotent ground snap (chantier 2 B1): world Y = terrain height at
// (x, z) + the reference's AUTHORED y (an offset above ground until the
// level editor writes real heights). Safe to re-run after every cell
// change; prefab-derived children (no base record) keep their expanded Y.
// Skipped for: interior cells (no terrain — authored y is absolute) and
// base forms with snapToGround = false (building modules on a pad).
void StreamingController::snapCellEntities(const StreamingContext& ctx) {
    // Entities changed (cell ring, travel, spawn): stale negative
    // collider verdicts go with them.
    nonCollidable.clear();
    if (ctx.editorOwnsTransforms) {
        return; // the editor owns transforms while it is active
    }
    const auto skipsSnap = [&](const world::ReferenceForm& reference,
                               data::FormHandle baseHandle) {
        if (const auto* cell = ctx.forms.find<world::CellForm>(reference.cell);
            cell && cell->interior) {
            return true;
        }
        const data::Form* base = ctx.forms.get(baseHandle);
        const reflect::TypeInfo* type = ctx.forms.typeOf(baseHandle);
        if (base && type) {
            if (const reflect::FieldInfo* field =
                    type->findField("snapToGround");
                field && field->kind == reflect::FieldKind::Bool &&
                !std::get<bool>(field->get(base))) {
                return true;
            }
        }
        return false;
    };
    ctx.world.handle()
        .query<world::Transform, const world::RefId,
               const world::MeshRender>()
        .each([&](flecs::entity, world::Transform& transform,
                  const world::RefId& ref, const world::MeshRender&) {
            const auto* reference =
                ctx.forms.find<world::ReferenceForm>(ref.referenceId);
            if (!reference || skipsSnap(*reference, ref.base)) {
                return;
            }
            transform.position.y =
                render::terrain::height(ctx.terrainParams,
                                        transform.position.x,
                                        transform.position.z) +
                reference->position.y;
        });
    // Lights too (no MeshRender): a torch's authored y is its height
    // above the ground it stands on.
    ctx.world.handle()
        .query<world::Transform, const world::RefId,
               const world::LightSource>()
        .each([&](flecs::entity, world::Transform& transform,
                  const world::RefId& ref, const world::LightSource&) {
            const auto* reference =
                ctx.forms.find<world::ReferenceForm>(ref.referenceId);
            if (!reference || skipsSnap(*reference, ref.base)) {
                return;
            }
            transform.position.y =
                render::terrain::height(ctx.terrainParams,
                                        transform.position.x,
                                        transform.position.z) +
                reference->position.y;
        });
}

// Chantier 3 B2: the navigator's obstacle set = the static colliders'
// world AABBs, inflated by the agent radius. Refreshed on cell changes.
void StreamingController::refreshNavObstacles(const StreamingContext& ctx) {
    if (!ctx.navigator || !ctx.meshCache) {
        return;
    }
    vector<world::TerrainNavigator::BlockingBox> boxes;
    ctx.world.handle()
        .query<const world::Transform, const world::MeshRender,
               const world::RefId>()
        .each([&](flecs::entity, const world::Transform& transform,
                  const world::MeshRender& mesh, const world::RefId& ref) {
            const data::Form* base = ctx.forms.get(ref.base);
            const reflect::TypeInfo* type = ctx.forms.typeOf(ref.base);
            if (!base || !type) {
                return;
            }
            const reflect::FieldInfo* field = type->findField("collides");
            if (!field || field->kind != reflect::FieldKind::Bool ||
                !std::get<bool>(field->get(base))) {
                return;
            }
            Vec3 lo { -0.5f }, hi { 0.5f };
            if (const MeshCache::CpuMesh* cpu =
                    ctx.meshCache->cpuMesh(mesh.model)) {
                lo = cpu->boundsMin;
                hi = cpu->boundsMax;
            }
            const Mat4 model =
                glm::translate(Mat4 { 1.0f }, transform.position) *
                glm::mat4_cast(transform.rotation) *
                glm::scale(Mat4 { 1.0f }, transform.scale);
            Vec3 wlo { 1e9f }, whi { -1e9f };
            for (u32 i = 0; i < 8; ++i) {
                const Vec3 corner { (i & 1) ? hi.x : lo.x,
                                    (i & 2) ? hi.y : lo.y,
                                    (i & 4) ? hi.z : lo.z };
                const Vec3 w = Vec3 { model * Vec4 { corner, 1.0f } };
                wlo = glm::min(wlo, w);
                whi = glm::max(whi, w);
            }
            constexpr f32 kAgentRadius = 0.4f;
            boxes.push_back({ wlo - Vec3 { kAgentRadius, 0.0f, kAgentRadius },
                              whi + Vec3 { kAgentRadius, 0.0f,
                                           kAgentRadius } });
        });
    ctx.navigator->setBlockingBoxes(std::move(boxes));
}

} // namespace game
