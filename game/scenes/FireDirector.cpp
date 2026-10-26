#include "game/scenes/FireDirector.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>

#include <glm/glm.hpp>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/VisualForms.hpp" // StaticForm, ParticleForm
#include "engine/physics/Physics.hpp"
#include "engine/render/SceneView.hpp" // RenderSnapshot, SceneLight
#include "engine/render/WorldRenderer.hpp"
#include "engine/render/landscape/TerrainNoise.hpp" // terrain::height
#include "engine/terrain/WaterBodies.hpp"
#include "engine/terrain/WindField.hpp"
#include "game/SoundResolver.hpp"
#include "game/scenes/NpcDirector.hpp" // Npc
#include "game/scenes/SpiritDirector.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/DerivedStats.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/StatsTuning.hpp"
#include "gameplay/stats/StatusBuildup.hpp"

namespace game {

vector<Vec2> FireDirector::placeFlames(const vector<Vec2>& burning,
                                       const Vec3& cam, const Vec3& forward3) {
    const Vec2 forward = glm::length(Vec2 { forward3.x, forward3.z }) > 1e-3f
                             ? glm::normalize(Vec2 { forward3.x, forward3.z })
                             : Vec2 { 0.0f, 1.0f };
    struct Candidate {
        Vec2 at;
        f32 score;
    };
    vector<Candidate> candidates;
    std::unordered_set<u64> farBlocks; // 2x2 blocks already represented
    for (const Vec2& cell : burning) {
        const Vec2 to { cell.x - cam.x, cell.y - cam.z };
        const f32 d2 = glm::dot(to, to);
        if (d2 > kFlameReach * kFlameReach) {
            continue;
        }
        const f32 dist = std::sqrt(d2);
        const f32 facing = dist > 1e-3f ? glm::dot(to / dist, forward) : 1.0f;
        if (facing < -0.2f && dist > kFlameBehindRadius) {
            continue; // behind the camera
        }
        Vec2 at = cell;
        if (dist > kFlameLodNear) {
            // LOD: the cell's 2x2 block centre, once per block.
            const i32 bx = static_cast<i32>(std::floor(cell.x / 4.0f));
            const i32 bz = static_cast<i32>(std::floor(cell.y / 4.0f));
            const u64 key = (static_cast<u64>(static_cast<u32>(bx)) << 32) ^
                            static_cast<u32>(bz);
            if (!farBlocks.insert(key).second) {
                continue;
            }
            at = { (static_cast<f32>(bx) + 0.5f) * 4.0f, (static_cast<f32>(bz) + 0.5f) * 4.0f };
        }
        // In the axis of view: preferred at equal distance.
        candidates.push_back({ at, dist * (1.6f - 0.6f * facing) });
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) {
                         return a.score < b.score;
                     });
    if (candidates.size() > kMaxFlames) {
        candidates.resize(kMaxFlames);
    }
    vector<Vec2> wanted;
    wanted.reserve(candidates.size());
    for (const Candidate& c : candidates) {
        wanted.push_back(c.at);
    }
    return wanted;
}

void FireDirector::update(const FireContext& ctx, f32 simSeconds) {
    SpiritDirector& spirits = ctx.spirits;
    if (spirits.fireIdle()) {
        return;
    }
    applyContact(ctx, simSeconds);
    // Burning props: they light the ground around them once, burn their
    // fuel, then are gone for good.
    for (size_t i = 0; i < burningProps.size();) {
        BurningProp& burning = burningProps[i];
        if (!burning.entity.is_alive()) {
            burningProps.erase(burningProps.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const Vec3 at = burning.entity.get<world::Transform>().position;
        if (!burning.lit) {
            spirits.ignite(at.x, at.z, 2.0f, 1.5f);
            burning.lit = true;
        }
        burning.remaining -= simSeconds;
        if (burning.remaining > 0.0f) {
            ++i;
            continue;
        }
        for (const u32 emitter : { burning.flames, burning.sparks, burning.smoke }) {
            if (emitter != 0) {
                ctx.fxSim.stopEmitter(emitter);
            }
        }
        ctx.cues.emit({ "Cue.Spirit.Fire.Spawn", at, 0.5f });
        ctx.retireProp(burning.entity);
        propFireHeat.erase(burning.entity.id());
        const ecs::Entity gone = burning.entity;
        burningProps.erase(burningProps.begin() + static_cast<std::ptrdiff_t>(i));
        gone.destruct();
    }
    const render::WaterSystem& water = ctx.renderer.waterSystem();
    SpiritDirector::FireFrame frame;
    frame.params = &ctx.terrainParams;
    frame.water = (water.simIsValid() && !water.simIsSettling())
                      ? water.simSnapshot()
                      : nullptr;
    frame.bodies = ctx.waterBodies;
    frame.seaLevel = ctx.terrainParams.seaLevel;
    frame.rain = ctx.rainIntensity; // the rain soaks the fuel and the flames
    const Vec3 cam = ctx.cameraPosition;
    frame.focus = { cam.x, cam.z };
    fireLandClock += simSeconds;
    spirits.setWindField(ctx.wind); // the job copies it
    if (!spirits.updateFire(ctx.jobs, frame, simSeconds)) {
        return;
    }
    const f32 landDt = fireLandClock;
    fireLandClock = 0.0f;
    // A job landed: the mask, the props, the trees and the flames follow it.
    updateProps(ctx);
    updateTrees(ctx, landDt);
    const render::terraingen::GridSpec& spec = spirits.fireSpec();
    ctx.renderer.fireScorchMap().upload(ctx.device, spirits.fireScorch(),
                                        spirits.fireGlow(), fireCanopy, spec.n,
                                        { spec.originX, spec.originZ },
                                        spec.texelSize);
    // Wanted flames: the set is re-placed every landed job, flames
    // burst in (placeFlames: the visible front, nearest first, under
    // budget).
    vector<Vec2> wanted = placeFlames(spirits.fireBurning(), cam, ctx.cameraForward);
    updateSound(ctx, cam);
    // Keep the emitters still wanted, stop the others, light the new.
    vector<FlameEmitter> kept;
    for (const FlameEmitter& flame : flameEmitters) {
        const auto it = std::find_if(wanted.begin(), wanted.end(),
                                     [&](const Vec2& at) {
                                         return std::abs(at.x - flame.at.x) < 0.01f &&
                                                std::abs(at.y - flame.at.y) < 0.01f;
                                     });
        if (it != wanted.end()) {
            kept.push_back(flame);
            wanted.erase(it);
        } else {
            for (const u32 emitter : { flame.emitter, flame.sparks, flame.smoke, flame.haze }) {
                if (emitter != 0) {
                    ctx.fxSim.stopEmitter(emitter);
                }
            }
        }
    }
    const auto* flameForm = ctx.forms.find<data::ParticleForm>(
        spirits.fieldParticles(render::terrain::SpiritKind::Fire));
    const auto* sparkForm = ctx.forms.find<data::ParticleForm>(
        spirits.sparkParticles(render::terrain::SpiritKind::Fire));
    const auto* smokeForm = ctx.forms.find<data::ParticleForm>(
        spirits.smokeParticles(render::terrain::SpiritKind::Fire));
    const auto* hazeForm = ctx.forms.find<data::ParticleForm>(
        spirits.hazeParticles(render::terrain::SpiritKind::Fire));
    for (const Vec2& at : wanted) {
        FlameEmitter flame { at, 0, 0, 0, 0 };
        const Vec3 pos { at.x,
                         render::terrain::height(ctx.terrainParams,
                                                 at.x, at.y) +
                             0.15f,
                         at.y };
        const u32 seed =
            static_cast<u32>(static_cast<i32>(pos.x * 73.0f)) ^
            (static_cast<u32>(static_cast<i32>(pos.z * 179.0f)) << 8) ^
            ctx.fxSim.count();
        const f32 dist = glm::distance(Vec2 { pos.x, pos.z }, Vec2 { cam.x, cam.z });
        const bool farFlame = dist > kFlameLodNear;
        if (flameForm) {
            fx::EmitterParams params = gameplay::toEmitterParams(*flameForm);
            params.duration = 1.0e9f; // stopped when the cell stops burning
            if (farFlame) {
                // One flame for four cells: taller, wider, a little sparser.
                params.sizeStart *= 1.8f;
                params.sizeEnd *= 1.8f;
                params.shapeRadius *= 2.0f;
                params.rate *= 0.7f;
            }
            flame.emitter = ctx.fxSim.spawn(params, pos, seed);
        }
        if (sparkForm) {
            fx::EmitterParams params = gameplay::toEmitterParams(*sparkForm);
            params.duration = 1.0e9f;
            flame.sparks = ctx.fxSim.spawn(params, pos, seed ^ 0x9e3779b9u);
        }
        // Discreet smoke: one cell in four (by its cell parity), rising
        // from above the flames and carried by the wind.
        const i32 cellX = static_cast<i32>(std::lround(at.x * 0.5f));
        const i32 cellZ = static_cast<i32>(std::lround(at.y * 0.5f));
        if (smokeForm && ((cellX + cellZ * 3) & 3) == 0) {
            fx::EmitterParams params = gameplay::toEmitterParams(*smokeForm);
            params.duration = 1.0e9f;
            flame.smoke = ctx.fxSim.spawn(params, pos + Vec3 { 0.0f, 1.2f, 0.0f },
                                          seed ^ 0x2545f491u);
        }
        // The heat shimmer above the near flames (a screen refraction:
        // pointless far away).
        if (hazeForm && dist < kFlameHazeNear) {
            fx::EmitterParams params = gameplay::toEmitterParams(*hazeForm);
            params.duration = 1.0e9f;
            flame.haze = ctx.fxSim.spawn(params, pos + Vec3 { 0.0f, 0.9f, 0.0f },
                                         seed ^ 0x7f4a7c15u);
        }
        kept.push_back(flame);
    }
    flameEmitters = std::move(kept);
}

void FireDirector::extractLights(const FireContext& ctx,
                                 render::RenderSnapshot& out) {
    const render::FireLook& look = ctx.renderer.fireScorchMap().look;
    if (look.lightIntensity <= 0.0f || look.lightCount <= 0 ||
        ctx.spirits.fireIdle()) {
        return;
    }
    // One tile = the cells of the front it holds (their centroid).
    struct Tile {
        Vec2 sum { 0.0f };
        u32 count { 0 };
        f32 glow { 0.0f }; // summed ember levels: the tile's freshness
    };
    std::map<std::pair<i32, i32>, Tile> tiles;
    for (const Vec2& at : ctx.spirits.fireBurning()) {
        const std::pair<i32, i32> key {
            static_cast<i32>(std::floor(at.x / kFireLightTile)),
            static_cast<i32>(std::floor(at.y / kFireLightTile))
        };
        Tile& tile = tiles[key];
        tile.sum += at;
        tile.glow += ctx.spirits.fireGlowAt(at.x, at.y);
        ++tile.count;
    }
    if (tiles.empty()) {
        return;
    }
    const Vec3 cam = ctx.cameraPosition;
    struct Candidate {
        render::SceneLight light;
        f32 dist2;
    };
    vector<Candidate> candidates;
    candidates.reserve(tiles.size());
    for (const auto& [key, tile] : tiles) {
        const Vec2 center = tile.sum / static_cast<f32>(tile.count);
        const f32 y =
            render::terrain::height(ctx.terrainParams, center.x, center.y) +
            0.8f;
        render::SceneLight light;
        light.position = { center.x, y, center.y };
        // Temperature: fresh flames burn the hot colour, a tile of dying
        // embers the cold one (the same ramp as the coal bed).
        const f32 freshness = glm::clamp(tile.glow / static_cast<f32>(tile.count), 0.0f, 1.0f);
        light.color = glm::mix(look.emberCold, look.lightColor, freshness);
        light.intensity = glm::min(look.lightIntensity * static_cast<f32>(tile.count),
                                   look.lightMaxIntensity);
        light.radius = look.lightRadius +
                       2.0f * std::sqrt(static_cast<f32>(tile.count));
        light.flicker = look.lightFlicker;
        const Vec3 to = light.position - cam;
        candidates.push_back({ light, glm::dot(to, to) });
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) {
                         return a.dist2 < b.dist2;
                     });
    const size_t keep =
        glm::min(candidates.size(), static_cast<size_t>(look.lightCount));
    // Ahead of the world's lights: the fire is the brightest thing
    // around; the budget's tail is what drops.
    vector<render::SceneLight> merged;
    merged.reserve(keep + out.lights.size());
    for (size_t i = 0; i < keep; ++i) {
        merged.push_back(candidates[i].light);
    }
    merged.insert(merged.end(), out.lights.begin(), out.lights.end());
    if (merged.size() > render::WorldRenderer::kMaxLights) {
        merged.resize(render::WorldRenderer::kMaxLights);
    }
    out.lights = std::move(merged);
}

void FireDirector::touch(const FireContext& ctx, ecs::Entity entity, f32& clock,
                         f32 dt, bool inFire) {
    using render::terrain::SpiritKind;
    if (!inFire || !entity.has<gameplay::AttributeSet>() ||
        !entity.has<gameplay::AbilitySystem>() ||
        !entity.has<gameplay::StatusBuildup>()) {
        clock = 0.0f;
        return;
    }
    clock += dt;
    if (clock < ctx.spirits.contactPeriod(SpiritKind::Fire)) {
        return;
    }
    clock = 0.0f;
    // The buildup (Status.Ignited in time) ...
    if (const auto* effect = ctx.forms.find<gameplay::EffectForm>(
            ctx.spirits.contactEffect(SpiritKind::Fire))) {
        gameplay::applyEffect(entity.get_mut<gameplay::AttributeSet>(),
                              entity.get_mut<gameplay::AbilitySystem>(), *effect,
                              ctx.tags, &entity.get_mut<gameplay::StatusBuildup>());
    }
    // ... and the flames' own typed damage through the pipeline (fire
    // resistance mitigates; §2.9's sanctioned terminal write).
    const f32 damage = ctx.spirits.contactDamage(SpiritKind::Fire);
    if (damage > 0.0f && entity.has<gameplay::CoreAttributes>() &&
        entity.has<gameplay::CombatState>()) {
        gameplay::StatBlock block {
            entity.get_mut<gameplay::CoreAttributes>(),
            entity.get_mut<gameplay::AttributeSet>(),
            entity.get_mut<gameplay::AbilitySystem>(),
            entity.get_mut<gameplay::CombatState>()
        };
        gameplay::DamageEvent burn;
        burn.channels = { { gameplay::DamageType::Fire, damage } };
        gameplay::applyDamage(block, burn, ctx.tags, ctx.derived, nullptr,
                              ctx.statsTuning);
    }
}

void FireDirector::applyContact(const FireContext& ctx, f32 dt) {
    if (ctx.playMode && ctx.policy.allowsSpirits &&
        ctx.playerEntity.is_alive() &&
        !ctx.casterWarded) { // the fire ward: no flame reaches the caster
        if (phys::CharacterBody* body = ctx.playerBody) {
            const Vec3 feet = body->position();
            touch(ctx, ctx.playerEntity, playerFireClock, dt,
                  ctx.spirits.fireBurningAt(feet.x, feet.z));
        }
    }
    if (ctx.policy.allowsSpirits) {
        for (const auto& npc : ctx.npcs) {
            if (!npc->entity.is_alive() || npc->dead ||
                !npc->entity.has<world::Transform>()) {
                continue;
            }
            const Vec3& at = npc->entity.get<world::Transform>().position;
            touch(ctx, npc->entity, npcFireClocks[npc->entity.id()], dt,
                  ctx.spirits.fireBurningAt(at.x, at.z) ||
                      ctx.jetTouched.contains(npc->entity.id()));
        }
    }
}

void FireDirector::updateProps(const FireContext& ctx) {
    // Heat every wooden prop standing in the glow (10 Hz: per landed job).
    const reflect::TypeInfo& staticType = data::StaticForm::staticTypeInfo();
    const world::SpiritRuleTable& rules = ctx.spirits.rules();
    const i32 woodIndex = rules.classIndex("wood");
    const f32 fuel = woodIndex >= 0 && static_cast<size_t>(woodIndex) < rules.props.size()
                         ? glm::max(rules.props[static_cast<size_t>(woodIndex)].fuel, 1.0f)
                         : 8.0f;
    ctx.interactQuery.each([&](flecs::entity e, const world::Transform& transform,
                               const world::RefId& ref) {
        const reflect::TypeInfo* type = ctx.forms.typeOf(ref.base);
        if (!type || !type->isA(staticType.id)) {
            return;
        }
        const auto* form = static_cast<const data::StaticForm*>(ctx.forms.get(ref.base));
        if (!form || form->surfaceMaterial != "wood") {
            return;
        }
        for (const BurningProp& burning : burningProps) {
            if (burning.entity == ecs::Entity { e }) {
                return; // already alight
            }
        }
        const bool inFlames = ctx.spirits.fireBurningAt(transform.position.x,
                                                        transform.position.z);
        f32& heat = propFireHeat[e.id()];
        if (!inFlames) {
            heat = glm::max(0.0f, heat - 0.1f * kPropHeatRate);
            return;
        }
        heat += 0.1f * kPropHeatRate;
        if (heat < 1.0f) {
            return;
        }
        lightProp(ctx, ecs::Entity { e }, transform.position, fuel);
    });
}

void FireDirector::lightProp(const FireContext& ctx, ecs::Entity entity,
                             const Vec3& position, f32 fuel) {
    using render::terrain::SpiritKind;
    const auto* flameForm = ctx.forms.find<data::ParticleForm>(
        ctx.spirits.fieldParticles(SpiritKind::Fire));
    const auto* sparkForm = ctx.forms.find<data::ParticleForm>(
        ctx.spirits.sparkParticles(SpiritKind::Fire));
    const auto* smokeForm = ctx.forms.find<data::ParticleForm>(
        ctx.spirits.smokeParticles(SpiritKind::Fire));
    BurningProp burning;
    burning.entity = entity;
    burning.remaining = fuel;
    const Vec3 at = position + Vec3 { 0.0f, 0.4f, 0.0f };
    const u32 seed = static_cast<u32>(entity.id() & 0xffffffffu) ^ 0x7a11beefu;
    if (flameForm) {
        fx::EmitterParams params = gameplay::toEmitterParams(*flameForm);
        params.duration = fuel;
        burning.flames = ctx.fxSim.spawn(params, at, seed);
    }
    if (sparkForm) {
        fx::EmitterParams params = gameplay::toEmitterParams(*sparkForm);
        params.duration = fuel;
        burning.sparks = ctx.fxSim.spawn(params, at, seed ^ 0x9e3779b9u);
    }
    if (smokeForm) {
        fx::EmitterParams params = gameplay::toEmitterParams(*smokeForm);
        params.duration = fuel;
        burning.smoke = ctx.fxSim.spawn(params, at + Vec3 { 0.0f, 1.0f, 0.0f },
                                        seed ^ 0x2545f491u);
    }
    ctx.cues.emit({ "Cue.Spirit.Fire.Spawn", at, 1.0f });
    burningProps.push_back(burning);
}

void FireDirector::updateTrees(const FireContext& ctx, f32 landDt) {
    using render::terrain::SpiritKind;
    const world::TreeFireParams& params = ctx.spirits.treeFire();
    const Vec3 cam = ctx.cameraPosition;
    // The trees near the camera (the vegetation's CPU copy), matched to
    // their state by their base.
    vector<render::VegetationSystem::GiProp> props;
    ctx.renderer.collectProps(cam, kTreeFireReach, props, 6000);
    const auto* flameForm = ctx.forms.find<data::ParticleForm>(
        ctx.spirits.fieldParticles(SpiritKind::Fire));
    const auto* sparkForm = ctx.forms.find<data::ParticleForm>(
        ctx.spirits.sparkParticles(SpiritKind::Fire));
    const auto* smokeForm = ctx.forms.find<data::ParticleForm>(
        ctx.spirits.smokeParticles(SpiritKind::Fire));
    for (const render::VegetationSystem::GiProp& prop : props) {
        if (prop.kind != 0) {
            continue; // trees only
        }
        // Exposure: the share of the 3x3 cells around the trunk that burn.
        u32 burning = 0;
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                if (ctx.spirits.fireBurningAt(prop.position.x + 2.0f * dx,
                                              prop.position.z + 2.0f * dz)) {
                    ++burning;
                }
            }
        }
        const f32 exposure =
            AmbientFxController::nearIgniter(ctx.igniters, prop.position)
                ? 1.0f
                : static_cast<f32>(burning) / 9.0f;
        const u64 key = treeKey(prop.position);
        auto it = treeFires.find(key);
        if (it == treeFires.end()) {
            if (exposure <= 0.0f) {
                continue; // cold and untouched: no state
            }
            it = treeFires.emplace(key, TreeFireEntry { prop.position, prop.scale }).first;
        }
        TreeFireEntry& tree = it->second;
        tree.at = prop.position;
        tree.scale = prop.scale;
        const bool caught = world::advanceTreeFire(tree.state, exposure, landDt, params);
        // Trunk and branches burn, and the crown with them: the scatter's
        // mean silhouette (~2.1 m of height per scale unit, the crown's
        // centre at 70 % of it, its radius ~40 % of it) places the
        // canopy's flames.
        const f32 height = 2.12f * tree.scale;
        const Vec3 trunk = tree.at + Vec3 { 0.0f, 2.0f * tree.scale, 0.0f };
        const Vec3 crown = tree.at + Vec3 { 0.0f, 0.7f * height, 0.0f };
        if (caught) {
            const u32 seed = static_cast<u32>(key ^ (key >> 32)) ^ 0x7ee5u;
            if (flameForm) {
                fx::EmitterParams p = gameplay::toEmitterParams(*flameForm);
                p.duration = params.burnSeconds;
                p.shapeRadius = 1.2f * tree.scale;
                p.sizeStart *= 1.6f * tree.scale;
                p.sizeEnd *= 1.6f * tree.scale;
                p.rate *= 1.5f;
                tree.flames = ctx.fxSim.spawn(p, trunk, seed);
                fx::EmitterParams c = gameplay::toEmitterParams(*flameForm);
                c.duration = params.burnSeconds;
                c.shape = fx::EmitterShape::Sphere;
                c.shapeRadius = 0.4f * height;
                c.sizeStart *= 1.4f;
                c.sizeEnd *= 1.4f;
                c.rate *= 3.0f;
                tree.crownFlames = ctx.fxSim.spawn(c, crown, seed ^ 0x51ed270bu);
            }
            if (sparkForm) {
                fx::EmitterParams p = gameplay::toEmitterParams(*sparkForm);
                p.duration = params.burnSeconds;
                p.shape = fx::EmitterShape::Sphere;
                p.shapeRadius = 0.45f * height;
                p.rate *= 3.0f;
                tree.sparks = ctx.fxSim.spawn(p, crown + Vec3 { 0.0f, 0.15f * height, 0.0f },
                                              seed ^ 0x9e3779b9u);
            }
            if (smokeForm) {
                fx::EmitterParams p = gameplay::toEmitterParams(*smokeForm);
                p.duration = params.burnSeconds;
                p.rate *= 2.0f;
                p.sizeStart *= 1.5f;
                p.sizeEnd *= 1.5f;
                tree.smoke = ctx.fxSim.spawn(p, crown + Vec3 { 0.0f, 0.3f * height, 0.0f },
                                             seed ^ 0x2545f491u);
            }
            ctx.cues.emit({ "Cue.Spirit.Fire.Spawn", crown, 1.0f });
        }
        if (tree.state.phase == world::TreeFirePhase::Burning) {
            // A burning tree sheds embers on the ground around it.
            tree.emberClock += landDt;
            if (tree.emberClock >= 3.0f) {
                tree.emberClock = 0.0f;
                ctx.spirits.ignite(tree.at.x, tree.at.z, 3.0f * tree.scale, 1.0f);
            }
        } else if (tree.flames != 0 || tree.crownFlames != 0 || tree.sparks != 0 ||
                   tree.smoke != 0) {
            for (u32* emitter : { &tree.flames, &tree.crownFlames, &tree.sparks, &tree.smoke }) {
                if (*emitter != 0) {
                    ctx.fxSim.stopEmitter(*emitter);
                    *emitter = 0;
                }
            }
        }
    }
    // Drop the states that went cold and whole; compose the canopy mask.
    const render::terraingen::GridSpec& spec = ctx.spirits.fireSpec();
    fireCanopy.assign(spec.cells(), 0);
    const i32 n = static_cast<i32>(spec.n);
    for (auto it = treeFires.begin(); it != treeFires.end();) {
        TreeFireEntry& tree = it->second;
        if (tree.state.idle()) {
            it = treeFires.erase(it);
            continue;
        }
        if (n > 0 && tree.state.burn > 0.0f) {
            const i32 col = static_cast<i32>(std::lround((tree.at.x - spec.originX) / spec.texelSize));
            const i32 row = static_cast<i32>(std::lround((tree.at.z - spec.originZ) / spec.texelSize));
            if (col >= 0 && row >= 0 && col < n && row < n) {
                u8& cell = fireCanopy[static_cast<size_t>(row) * n + col];
                cell = glm::max(cell, static_cast<u8>(tree.state.burn * 255.0f + 0.5f));
            }
        }
        ++it;
    }
}

void FireDirector::updateSound(const FireContext& ctx, const Vec3& cam) {
    // The nearest burning cell within reach carries the loop.
    const Vec2* nearest = nullptr;
    f32 best = kFireSoundReach * kFireSoundReach;
    for (const Vec2& at : ctx.spirits.fireBurning()) {
        const f32 dx = at.x - cam.x;
        const f32 dz = at.y - cam.z;
        const f32 d2 = dx * dx + dz * dz;
        if (d2 < best) {
            best = d2;
            nearest = &at;
        }
    }
    if (!nearest) {
        if (fireLoop != 0) {
            ctx.audio.stop(fireLoop, 0.8f);
            fireLoop = 0;
        }
        return;
    }
    const Vec3 at { nearest->x,
                    render::terrain::height(ctx.terrainParams, nearest->x,
                                            nearest->y) +
                        0.5f,
                    nearest->y };
    if (fireLoop == 0) {
        const core::Guid& sound =
            ctx.spirits.fieldSound(render::terrain::SpiritKind::Fire);
        if (const auto params = ctx.sounds.resolve(sound, at, 0x5f1e5u)) {
            fireLoop = ctx.audio.play(*params);
        }
        return;
    }
    ctx.audio.setPosition(fireLoop, at);
}

void FireDirector::applyIgniters(const FireContext& ctx, f32 dt) {
    const vector<AmbientFxController::Igniter>& igniters = ctx.igniters;
    if (igniters.empty() || !ctx.policy.allowsSpirits) {
        return;
    }
    // The ground under a low flame: heat for the next job.
    for (const AmbientFxController::Igniter& igniter : igniters) {
        if (igniter.ground) {
            ctx.spirits.ignite(igniter.at.x, igniter.at.z, igniter.radius,
                               igniter.heat * dt);
        }
    }
    // Wooden props in a flame heat up as if in the fire.
    const reflect::TypeInfo& staticType = data::StaticForm::staticTypeInfo();
    const world::SpiritRuleTable& rules = ctx.spirits.rules();
    const i32 woodIndex = rules.classIndex("wood");
    const f32 fuel = woodIndex >= 0 && static_cast<size_t>(woodIndex) < rules.props.size()
                         ? glm::max(rules.props[static_cast<size_t>(woodIndex)].fuel, 1.0f)
                         : 8.0f;
    bool lit = false;
    ctx.interactQuery.each([&](flecs::entity e, const world::Transform& transform,
                               const world::RefId& ref) {
        if (!AmbientFxController::nearIgniter(igniters, transform.position)) {
            return;
        }
        const reflect::TypeInfo* type = ctx.forms.typeOf(ref.base);
        if (!type || !type->isA(staticType.id)) {
            return;
        }
        const auto* form = static_cast<const data::StaticForm*>(ctx.forms.get(ref.base));
        if (!form || form->surfaceMaterial != "wood") {
            return;
        }
        for (const BurningProp& burning : burningProps) {
            if (burning.entity == ecs::Entity { e }) {
                return;
            }
        }
        f32& heat = propFireHeat[e.id()];
        heat += kPropHeatRate * dt;
        if (heat >= 1.0f) {
            lightProp(ctx, ecs::Entity { e }, transform.position, fuel);
            lit = true;
        }
    });
    // Trees in a flame: the lane's job ticks them (updateTrees reads the
    // igniters), so it must run — checked once a second.
    igniterTreeClock -= dt;
    if (igniterTreeClock <= 0.0f) {
        igniterTreeClock = 1.0f;
        igniterNearTree = false;
        vector<render::VegetationSystem::GiProp> props;
        ctx.renderer.collectProps(ctx.cameraPosition,
                                  AmbientFxController::kSourceReach, props, 6000);
        for (const auto& prop : props) {
            if (prop.kind == 0 &&
                AmbientFxController::nearIgniter(igniters, prop.position)) {
                igniterNearTree = true;
                break;
            }
        }
    }
    if (lit || igniterNearTree || !burningProps.empty()) {
        ctx.spirits.wakeFire();
    }
}

void FireDirector::reset(const FireContext& ctx) {
    if (fireLoop != 0) {
        ctx.audio.stop(fireLoop, 0.3f);
        fireLoop = 0;
    }
    for (const BurningProp& burning : burningProps) {
        for (const u32 emitter : { burning.flames, burning.sparks, burning.smoke }) {
            if (emitter != 0) {
                ctx.fxSim.stopEmitter(emitter);
            }
        }
    }
    burningProps.clear();
    propFireHeat.clear();
    for (auto& [key, tree] : treeFires) {
        for (const u32 emitter : { tree.flames, tree.crownFlames, tree.sparks, tree.smoke }) {
            if (emitter != 0) {
                ctx.fxSim.stopEmitter(emitter);
            }
        }
    }
    treeFires.clear();
    fireCanopy.clear();
    fireLandClock = 0.0f;
    npcFireClocks.clear();
    playerFireClock = 0.0f;
    for (const FlameEmitter& flame : flameEmitters) {
        for (const u32 emitter : { flame.emitter, flame.sparks, flame.smoke, flame.haze }) {
            if (emitter != 0) {
                ctx.fxSim.stopEmitter(emitter);
            }
        }
    }
    flameEmitters.clear();
    ctx.spirits.resetFire();
    ctx.renderer.fireScorchMap().clear(ctx.device);
    igniterNearTree = false;
    igniterTreeClock = 0.0f;
}

} // namespace game
