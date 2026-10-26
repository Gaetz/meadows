#include "game/scenes/SpiritDirector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "data/forms/FormQuery.hpp"
#include "data/forms/LocForms.hpp"    // TextTable
#include "data/forms/VisualForms.hpp" // ParticleForm, StaticForm
#include "engine/core/Log.hpp"
#include "engine/fx/Particles.hpp"
#include "engine/physics/Physics.hpp"
#include "engine/render/MeshCache.hpp"
#include "engine/render/WorldRenderer.hpp"
#include "engine/render/landscape/TerrainSystem.hpp" // kChunkSize
#include "engine/terrain/WaterQuery.hpp"
#include "game/scenes/AmbientFxController.hpp"
#include "game/scenes/NpcDirector.hpp" // Npc
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/cue/GameplayCues.hpp" // toEmitterParams
#include "world/spirit/SpiritWaterMesh.hpp"

namespace game {

// The earth gestures: the bump, the dig and the stone brush, the seized
// rocks, the wall, the ground reading (docs/CHANTIER-ESPRITS.md).

f32 SpiritDirector::groundHardnessAt(const SpiritContext& ctx, f32 x, f32 z) const {
    // The dominant splat names the material class; the compiled rule
    // table carries its hardness (SurfaceMaterialForm, data).
    const auto& params = ctx.terrainParams;
    const f32 h = render::terrain::height(params, x, z);
    const render::terrain::MaterialWeights w = render::terrain::materialWeightsAt(
        params, x, z, h, render::terrain::normal(params, x, z));
    const char* cls = "grass";
    f32 best = w.grass;
    if (w.rock > best) { best = w.rock; cls = "rock"; }
    if (w.cliff > best) { best = w.cliff; cls = "cliff"; }
    if (w.snow > best) { best = w.snow; cls = "snow"; }
    if (w.sand > best) { best = w.sand; cls = "sand"; }
    const world::SpiritRuleTable& ruleTable = rules();
    const i32 index = ruleTable.classIndex(cls);
    if (index < 0 || static_cast<size_t>(index) >= ruleTable.props.size()) {
        return 1.0f;
    }
    return glm::max(ruleTable.props[static_cast<size_t>(index)].hardness, 0.1f);
}

void SpiritDirector::applyEarthBump(const SpiritContext& ctx,
                                    const PendingAction& action) {
    // The mound: one instant raise of `rate` metres (divided by the
    // ground's hardness) over the spell's radius, published committed.
    const f32 height = action.rate / groundHardnessAt(ctx, action.x, action.z);
    if (height <= 0.0f) {
        return;
    }
    // What stands on it is thrown by the republish funnel
    // (throwActorsOnGroundRise), like any rising ground.
    const Vec2 center { action.x, action.z };
    world::BrushGrids grids;
    world::BrushParams brush;
    brush.kind = world::BrushKind::Raise;
    brush.radius = action.radius;
    brush.strength = height;
    world::applyTerrainBrush(grids, ctx.heightPatches.get(),
                             render::TerrainSystem::kChunkSize, brush, center,
                             1.0f);
    vector<u64> changed;
    auto next = world::publishBrushGrids(grids, ctx.heightPatches.get(),
                                         render::TerrainSystem::kChunkSize,
                                         changed);
    ctx.republishTerrain(std::move(next), changed, true);
    const Vec3 at { action.x,
                    render::terrain::height(ctx.terrainParams, action.x,
                                            action.z),
                    action.z };
    ctx.cues.emit({ "Cue.Spirit.Earth.Spawn", at, height });
}

void SpiritDirector::updateEarth(const SpiritContext& ctx, f32 dt) {
    if (!spiritEarth) {
        return;
    }
    SpiritEarth& dig = *spiritEarth;
    const bool held = ctx.castHeld;
    dig.remaining -= dt;
    if (!held || dig.remaining <= 0.0f || !ctx.playerEntity.is_alive()) {
        finishEarth(ctx, true);
        return;
    }
    dig.costClock += dt;
    if (dig.costClock >= dig.costPeriod) {
        dig.costClock -= dig.costPeriod;
        const auto* ability = ctx.forms.find<gameplay::AbilityForm>(dig.ability);
        auto& set = ctx.playerEntity.get_mut<gameplay::AttributeSet>();
        auto& system = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
        if (ability && !gameplay::payAbilityCost(*ability, set, system,
                                                 { ctx.forms, ctx.tags },
                                                 dig.upkeepScale)) {
            ctx.say(ctx.texts.get("spirit.refused"), 1.5f);
            finishEarth(ctx, true);
            return;
        }
    }
    const std::optional<Vec3> at = ctx.aimGround();
    if (!at) {
        return;
    }
    world::BrushParams brush;
    brush.kind = dig.raise ? world::BrushKind::Raise : world::BrushKind::Lower;
    brush.radius = dig.radius;
    brush.strength = dig.rate / groundHardnessAt(ctx, at->x, at->z);
    world::applyTerrainBrush(dig.grids, ctx.heightPatches.get(),
                             render::TerrainSystem::kChunkSize, brush,
                             { at->x, at->z }, dt);
    // Live preview at ~20 Hz (terrain only, the water reacts); the heavy
    // commit waits for the release.
    dig.previewTimer += dt;
    if (dig.previewTimer >= 0.05f) {
        dig.previewTimer = 0.0f;
        vector<u64> changed;
        auto next = world::publishBrushGrids(dig.grids, ctx.heightPatches.get(),
                                             render::TerrainSystem::kChunkSize,
                                             changed);
        ctx.republishTerrain(std::move(next), changed, false);
    }
    dig.cueTimer += dt;
    if (dig.cueTimer >= 0.3f) {
        dig.cueTimer = 0.0f;
        ctx.cues.emit(
            { dig.raise ? "Cue.Spirit.Earth.Spawn" : "Cue.Spirit.Earth.Dig",
              *at, dig.rate });
    }
}

void SpiritDirector::finishEarth(const SpiritContext& ctx, bool commit) {
    if (!spiritEarth) {
        return;
    }
    SpiritEarth dig = std::move(*spiritEarth);
    spiritEarth.reset();
    if (!commit || dig.grids.empty()) {
        if (!commit && !dig.grids.empty()) {
            // Abandoned mid-stroke: the preview overlay goes back to the
            // committed one.
            ctx.terrainParams.patches = ctx.heightPatches;
        }
        return;
    }
    vector<u64> changed;
    auto next = world::publishBrushGrids(dig.grids, ctx.heightPatches.get(),
                                         render::TerrainSystem::kChunkSize,
                                         changed);
    ctx.republishTerrain(std::move(next), changed, true);
}

void SpiritDirector::seizeRock(const SpiritContext& ctx,
                               const PendingAction& action) {
    // The nearest rock prop (StaticForm.surfaceMaterial "rock") within
    // reach of the aim, no larger than the spell's intensity in metres
    // (its bounding radius) — a static body handed to the spirit and
    // re-created as a dynamic convex hull.
    const reflect::TypeInfo& staticType = data::StaticForm::staticTypeInfo();
    const Vec2 aim { action.x, action.z };
    ecs::Entity best;
    f32 bestDist = action.radius;
    f32 bestRadius = 0.0f;
    ctx.interactQuery.each([&](flecs::entity e, const world::Transform& transform,
                           const world::RefId& ref) {
        const reflect::TypeInfo* type = ctx.forms.typeOf(ref.base);
        if (!type || !type->isA(staticType.id) ||
            !e.has<world::MeshRender>()) {
            return;
        }
        const auto* form = static_cast<const data::StaticForm*>(ctx.forms.get(ref.base));
        if (!form || form->surfaceMaterial != "rock") {
            return;
        }
        for (const SpiritRock& rock : spiritRocks) {
            if (rock.entity == ecs::Entity { e }) {
                return; // already ours (free or held)
            }
        }
        const render::MeshCache::CpuMesh* cpu =
            ctx.meshes->cpuMesh(e.get<world::MeshRender>().model);
        if (!cpu) {
            return;
        }
        const Vec3 half = (cpu->boundsMax - cpu->boundsMin) * 0.5f *
                          transform.scale;
        const f32 radius = glm::max(half.x, glm::max(half.y, half.z));
        if (radius > action.rate) {
            return; // too big for this spell
        }
        const f32 dist = glm::distance(
            Vec2 { transform.position.x, transform.position.z }, aim);
        if (dist < bestDist) {
            bestDist = dist;
            best = ecs::Entity { e };
            bestRadius = radius;
        }
    });
    if (!best.is_alive()) {
        ctx.say(ctx.texts.get("spirit.noRock"), 1.5f);
        return;
    }
    const auto& transform = best.get<world::Transform>();
    const render::MeshCache::CpuMesh* cpu =
        ctx.meshes->cpuMesh(best.get<world::MeshRender>().model);
    const Vec3 extent = (cpu->boundsMax - cpu->boundsMin) * transform.scale;
    const f32 mass = glm::max(extent.x * extent.y * extent.z, 0.05f) * 2000.0f;
    const phys::BodyId body = ctx.physics->addDynamicConvex(
        cpu->positions.data(), static_cast<u32>(cpu->positions.size()),
        transform.position, transform.rotation, transform.scale, mass);
    if (body == 0) {
        return;
    }
    ctx.retireCollider(best);
    ctx.physics->setKinematic(body, true);
    best.set<world::Displaced>({ transform.position.y -
                                 render::terrain::height(ctx.terrainParams,
                                                         transform.position.x,
                                                         transform.position.z) });
    SpiritRock rock;
    rock.entity = best;
    rock.body = body;
    rock.held = true;
    rock.radius = bestRadius;
    rock.lastTarget = transform.position;
    rock.costPeriod = action.costPeriod;
    rock.upkeepScale = action.upkeepScale;
    rock.ability = spiritAbility ? spiritAbility->id : core::Guid {};
    spiritRocks.push_back(rock);
    ctx.cues.emit({ "Cue.Spirit.Earth.Dig", transform.position, 1.0f });
}

void SpiritDirector::updateRocks(const SpiritContext& ctx, f32 dt) {
    const bool held = ctx.castHeld;
    for (SpiritRock& rock : spiritRocks) {
        if (!rock.held || !rock.entity.is_alive()) {
            continue;
        }
        bool release = !held || !ctx.playerEntity.is_alive();
        if (!release) {
            rock.costClock += dt;
            if (rock.costClock >= rock.costPeriod) {
                rock.costClock -= rock.costPeriod;
                const auto* ability = ctx.forms.find<gameplay::AbilityForm>(rock.ability);
                auto& set = ctx.playerEntity.get_mut<gameplay::AttributeSet>();
                auto& system = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
                if (ability && !gameplay::payAbilityCost(*ability, set, system,
                                                         { ctx.forms, ctx.tags },
                                                         rock.upkeepScale)) {
                    ctx.say(ctx.texts.get("spirit.refused"), 1.5f);
                    release = true;
                }
            }
        }
        if (release) {
            // Let go: dynamic again, thrown with the carry velocity.
            rock.held = false;
            ctx.physics->setKinematic(rock.body, false);
            const Vec3 velocity =
                dt > 1e-4f ? (rock.lastTarget - rock.entity.get<world::Transform>().position) / dt
                           : Vec3 { 0.0f };
            ctx.physics->setLinearVelocity(rock.body, velocity);
            continue;
        }
        // Carried over the aim (or ahead of the eye when nothing is aimed):
        // the body is driven kinematically so it shoves what it meets.
        const std::optional<Vec3> at = ctx.aimGround();
        Vec3 target = at ? *at + Vec3 { 0.0f, 2.5f + rock.radius, 0.0f }
                         : ctx.cameraPosition +
                               ctx.cameraForward * (6.0f + rock.radius);
        const Vec3 previous = rock.lastTarget;
        rock.lastTarget = target;
        const Quat rotation = rock.entity.get<world::Transform>().rotation;
        ctx.physics->moveKinematic(rock.body, target, rotation, dt);
        (void)previous;
    }
}

void SpiritDirector::releaseRocks(const SpiritContext& ctx) {
    if (ctx.physics) {
        for (const SpiritRock& rock : spiritRocks) {
            ctx.physics->removeBody(rock.body);
        }
    }
    spiritRocks.clear();
}

void SpiritDirector::syncRocks(phys::PhysicsWorld& physics,
                               const render::TerrainParams& terrain) {
    for (auto it = spiritRocks.begin(); it != spiritRocks.end();) {
        if (!it->entity.is_alive()) {
            physics.removeBody(it->body); // its cell unloaded
            it = spiritRocks.erase(it);
            continue;
        }
        const phys::PhysicsWorld::BodyPose pose = physics.bodyPose(it->body);
        auto& transform = it->entity.get_mut<world::Transform>();
        transform.position = pose.position;
        transform.rotation = pose.rotation;
        if (it->entity.has<world::Displaced>()) {
            it->entity.get_mut<world::Displaced>().groundOffsetY =
                pose.position.y -
                render::terrain::height(terrain, pose.position.x, pose.position.z);
        }
        ++it;
    }
}

void SpiritDirector::updateLine(const SpiritContext& ctx) {
    if (!spiritLine) {
        return;
    }
    if (ctx.castHeld &&
        ctx.playerEntity.is_alive()) {
        return; // still drawing
    }
    const SpiritLine line = *spiritLine;
    spiritLine.reset();
    const std::optional<Vec3> end = ctx.aimGround();
    if (!end) {
        return;
    }
    // The wall: `height` metres over the hardness of the ground at its
    // middle, from the press spot to the release spot.
    const Vec2 a { line.start.x, line.start.z };
    const Vec2 b { end->x, end->z };
    const Vec2 mid = (a + b) * 0.5f;
    const f32 height = line.height / groundHardnessAt(ctx, mid.x, mid.y);
    world::BrushGrids grids;
    world::applyTerrainWall(grids, ctx.heightPatches.get(),
                            render::TerrainSystem::kChunkSize, a, b,
                            line.halfWidth, height);
    vector<u64> changed;
    auto next = world::publishBrushGrids(grids, ctx.heightPatches.get(),
                                         render::TerrainSystem::kChunkSize,
                                         changed);
    ctx.republishTerrain(std::move(next), changed, true);
    const Vec3 at { mid.x,
                    render::terrain::height(ctx.terrainParams, mid.x,
                                            mid.y),
                    mid.y };
    ctx.cues.emit({ "Cue.Spirit.Earth.Spawn", at, height });
}

void SpiritDirector::castGroundReading(const SpiritContext& ctx, const Vec3& at,
                                       f32 seconds, bool live) {
    const auto& params = ctx.terrainParams;
    const f32 h = render::terrain::height(params, at.x, at.z);
    const Vec3 n = render::terrain::normal(params, at.x, at.z);
    const render::terrain::MaterialWeights w =
        render::terrain::materialWeightsAt(params, at.x, at.z, h, n);
    const render::terrain::RegionFields fields =
        render::terrain::regionFieldsAt(params, at.x, at.z);
    const char* cls = "grass";
    f32 best = w.grass;
    if (w.rock > best) { best = w.rock; cls = "rock"; }
    if (w.cliff > best) { best = w.cliff; cls = "cliff"; }
    if (w.snow > best) { best = w.snow; cls = "snow"; }
    if (w.sand > best) { best = w.sand; cls = "sand"; }
    const world::SpiritRuleTable& ruleTable = rules();
    world::MaterialProps props;
    if (const i32 index = ruleTable.classIndex(cls);
        index >= 0 && static_cast<size_t>(index) < ruleTable.props.size()) {
        props = ruleTable.props[static_cast<size_t>(index)];
    }
    const f32 slopeDeg =
        glm::degrees(std::acos(glm::clamp(n.y, -1.0f, 1.0f)));
    const auto num = [](f32 v, i32 decimals) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), decimals == 0 ? "%.0f" : "%.1f",
                      static_cast<f64>(v));
        return str { buf };
    };
    vector<str> lines;
    lines.push_back(ctx.texts.get("ground.title"));
    lines.push_back(ctx.texts.format("ground.material",
                                 { ctx.texts.get(str { "material." } + cls),
                                   num(props.hardness, 1) }));
    lines.push_back(ctx.texts.format("ground.relief",
                                 { num(slopeDeg, 0), num(h, 0) }));
    lines.push_back(ctx.texts.format(
        "ground.climate",
        { num(glm::clamp(fields.biomeWetness, 0.0f, 1.0f) * 100.0f, 0),
          num(fields.temperature, 1) }));
    lines.push_back(ctx.texts.format(
        "ground.fire", { num(props.flammability * 100.0f, 0),
                         num(props.moisture * 100.0f, 0) }));
    const bool opening = !spiritReadingLive && spiritReadingSeconds <= 0.0f;
    spiritReadingKind = ReadingKind::Ground;
    spiritReading = std::move(lines);
    spiritReading.resize(kReadingLines);
    spiritReadingLive = live;
    spiritReadingSeconds = live ? 0.0f : glm::max(seconds, 1.0f);
    if (opening) {
        ctx.cues.emit({ "Cue.Spirit.Earth.Dig", at, 1.0f });
    }
}

} // namespace game
