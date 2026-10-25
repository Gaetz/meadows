#include "game/scenes/AmbientFxController.hpp"

#include <cmath>
#include <unordered_set>

#include <glm/glm.hpp>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/LandscapeForms.hpp" // RiverSourceFxForm
#include "data/forms/VisualForms.hpp"    // ParticleForm
#include "engine/core/Log.hpp"
#include "engine/render/landscape/TerrainNoise.hpp" // terrain::height
#include "engine/terrain/WaterBodies.hpp"
#include "engine/terrain/WindField.hpp"
#include "engine/terrain/generation/MasterNetwork.hpp" // RiverHeadType
#include "game/SoundResolver.hpp"
#include "game/scenes/SpiritDirector.hpp"
#include "gameplay/cue/GameplayCues.hpp" // toEmitterParams

namespace game {

bool AmbientFxController::nearIgniter(const vector<Igniter>& igniters,
                                      const Vec3& at) {
    for (const Igniter& igniter : igniters) {
        const f32 dx = at.x - igniter.at.x;
        const f32 dz = at.z - igniter.at.z;
        const f32 reach = igniter.radius + 0.5f;
        if (dx * dx + dz * dz <= reach * reach) {
            return true;
        }
    }
    return false;
}

void AmbientFxController::update(const AmbientFxContext& ctx) {
    updateSources(ctx);      // the torches and campfires within reach
    updateRiverSources(ctx); // mist and murmur at the springs in reach
    updateRiverCascades(ctx); // spray and roar at the falls in reach
}

void AmbientFxController::snuffSource(const AmbientFxContext& ctx,
                                      ecs::Entity entity,
                                      world::FxSource& source, const Vec3& at) {
    source.lit = false;
    if (auto it = sourceEmitters.find(entity.id()); it != sourceEmitters.end()) {
        for (const u32 emitter : { it->second.flame, it->second.haze }) {
            if (emitter != 0) {
                ctx.fxSim.stopEmitter(emitter);
            }
        }
        if (it->second.sound != 0) {
            ctx.audio.stop(it->second.sound, 0.3f);
        }
        if (it->second.smoke != 0) {
            ctx.fxSim.stopEmitter(it->second.smoke);
        }
        sourceEmitters.erase(it);
    }
    // The last breath: a short, thicker puff of its own smoke.
    if (const auto* form = ctx.forms.find<data::ParticleForm>(source.smoke)) {
        fx::EmitterParams params = gameplay::toEmitterParams(*form);
        params.duration = 1.5f;
        params.rate *= 4.0f;
        ctx.fxSim.spawn(params, at, static_cast<u32>(entity.id() & 0xffffffffu) ^ 0x51u);
    }
    if (entity.has<world::LightSource>()) {
        entity.get_mut<world::LightSource>().litScale = 0.0f;
    }
}

void AmbientFxController::updateRiverSources(const AmbientFxContext& ctx) {
    // Mist (and an optional loop) at the TRUE head of each published
    // course, by its source typology — same reach/diff pattern as the
    // fire FxSources. Skipped indoors (the heads are outdoor data).
    const auto sweep = [&](const std::unordered_set<u64>& seen) {
        for (auto it = riverSourceEmitters.begin();
             it != riverSourceEmitters.end();) {
            if (seen.contains(it->first)) {
                ++it;
                continue;
            }
            if (it->second.haze != 0) {
                ctx.fxSim.stopEmitter(it->second.haze);
            }
            if (it->second.sound != 0) {
                ctx.audio.stop(it->second.sound, 0.5f);
            }
            it = riverSourceEmitters.erase(it);
        }
    };
    std::unordered_set<u64> seen;
    if (!ctx.waterBodies || !ctx.policy.hasWaterBodies) {
        sweep(seen);
        return;
    }
    const data::RiverSourceFxForm* dress = nullptr;
    data::forEach<data::RiverSourceFxForm>(
        ctx.forms, [&](const data::RiverSourceFxForm& f) {
            if (!dress) {
                dress = &f;
            }
        });
    if (!dress) {
        sweep(seen);
        return;
    }
    const Vec3 cam = ctx.cameraPosition;
    using render::terraingen::RiverHeadType;
    for (const render::RiverSurface& river : ctx.waterBodies->rivers) {
        if (!river.headIsSource || river.nodes.empty() ||
            river.headType ==
                static_cast<u32>(RiverHeadType::LakeOutlet)) {
            continue;
        }
        const render::RiverNode& head = river.nodes.front();
        const Vec3 at { head.x, head.surface + 0.2f, head.z };
        if (glm::distance(at, cam) > kSourceReach) {
            continue;
        }
        const u64 key =
            (static_cast<u64>(static_cast<u32>(
                 static_cast<i32>(head.x * 4.0f)))
             << 32) |
            static_cast<u64>(
                static_cast<u32>(static_cast<i32>(head.z * 4.0f)));
        seen.insert(key);
        if (riverSourceEmitters.contains(key)) {
            continue;
        }
        core::Guid mist;
        core::Guid sound;
        switch (static_cast<RiverHeadType>(river.headType)) {
        case RiverHeadType::Neve:
            mist = dress->mistNeve;
            sound = dress->soundNeve;
            break;
        case RiverHeadType::Glacier:
            mist = dress->mistGlacier;
            sound = dress->soundGlacier;
            break;
        default:
            mist = dress->mistSpring;
            sound = dress->soundSpring;
            break;
        }
        Emitters emitters;
        const u32 seed = static_cast<u32>(key & 0xffffffffu) ^
                         0x5ee0f1a3u;
        if (const auto* form = ctx.forms.find<data::ParticleForm>(mist)) {
            fx::EmitterParams params = gameplay::toEmitterParams(*form);
            params.duration = 1.0e9f; // for the source's life
            emitters.haze = ctx.fxSim.spawn(params, at, seed);
        }
        if (sound.isValid()) {
            if (const auto params =
                    ctx.sounds.resolve(sound, at, seed)) {
                emitters.sound = ctx.audio.play(*params);
            }
        }
        if (emitters.haze != 0 || emitters.sound != 0) {
            riverSourceEmitters.emplace(key, emitters);
        }
    }
    sweep(seen);
}

void AmbientFxController::updateRiverCascades(const AmbientFxContext& ctx) {
    // Impact spray (and an optional roar) at the FOOT of each fall:
    // a run of steep ribbon segments is a cascade — detected from the
    // published nodes, no baked marker needed. Same reach/diff/sweep
    // pattern as updateRiverSources; a missing cascade record falls
    // back to the spring dressing so the falls read even before the
    // dedicated particles are authored.
    const auto sweep = [&](const std::unordered_set<u64>& seen) {
        for (auto it = riverCascadeEmitters.begin();
             it != riverCascadeEmitters.end();) {
            if (seen.contains(it->first)) {
                ++it;
                continue;
            }
            if (it->second.haze != 0) {
                ctx.fxSim.stopEmitter(it->second.haze);
            }
            if (it->second.sound != 0) {
                ctx.audio.stop(it->second.sound, 0.5f);
            }
            it = riverCascadeEmitters.erase(it);
        }
    };
    std::unordered_set<u64> seen;
    if (!ctx.waterBodies || !ctx.policy.hasWaterBodies) {
        sweep(seen);
        return;
    }
    const data::RiverSourceFxForm* dress = nullptr;
    data::forEach<data::RiverSourceFxForm>(
        ctx.forms, [&](const data::RiverSourceFxForm& f) {
            if (!dress) {
                dress = &f;
            }
        });
    if (!dress) {
        sweep(seen);
        return;
    }
    const core::Guid mist = dress->mistCascade.isValid()
                                ? dress->mistCascade
                                : dress->mistSpring;
    const core::Guid sound = dress->soundCascade.isValid()
                                 ? dress->soundCascade
                                 : dress->soundSpring;
    const Vec3 cam = ctx.cameraPosition;
    constexpr f32 kFallSlope = 0.25f; // rise/run along the ribbon
    constexpr f32 kFallDropMin = 3.0f;
    // A waterfall reads much further than a spring wisp.
    constexpr f32 kFxCascadeReach = 250.0f;
    for (const render::RiverSurface& river : ctx.waterBodies->rivers) {
        const auto& nodes = river.nodes;
        size_t i = 0;
        while (i + 1 < nodes.size()) {
            // Chain the steep segments of one fall.
            f32 drop = 0.0f;
            size_t foot = i;
            size_t k = i;
            while (k + 1 < nodes.size()) {
                const f32 d = nodes[k].surface - nodes[k + 1].surface;
                const f32 run = glm::max(
                    std::hypot(nodes[k + 1].x - nodes[k].x,
                               nodes[k + 1].z - nodes[k].z),
                    0.001f);
                if (d / run < kFallSlope) {
                    break;
                }
                drop += d;
                foot = k + 1;
                ++k;
            }
            if (drop < kFallDropMin) {
                ++i;
                continue;
            }
            const render::RiverNode& fn = nodes[foot];
            const Vec3 at { fn.x, fn.surface + 0.5f, fn.z };
            const size_t lip = i;
            i = foot + 1;
            if (glm::distance(at, cam) > kFxCascadeReach) {
                continue;
            }
            // A steep profile whose water the fine ground CONTRADICTS
            // is a buried wall reach, not a visible fall (the
            // re-route backlog owns those): no spray on dry rock.
            if (render::terrain::height(ctx.terrain, fn.x, fn.z) >
                    fn.surface + 2.0f ||
                render::terrain::height(ctx.terrain, nodes[lip].x,
                                        nodes[lip].z) >
                    nodes[lip].surface + 2.0f) {
                continue;
            }
            const u64 key =
                (static_cast<u64>(static_cast<u32>(
                     static_cast<i32>(fn.x * 4.0f)))
                 << 32) |
                static_cast<u64>(
                    static_cast<u32>(static_cast<i32>(fn.z * 4.0f)));
            seen.insert(key);
            if (riverCascadeEmitters.contains(key)) {
                continue;
            }
            Emitters emitters;
            const u32 seed =
                static_cast<u32>(key & 0xffffffffu) ^ 0xca5cade5u;
            if (const auto* form =
                    ctx.forms.find<data::ParticleForm>(mist)) {
                fx::EmitterParams params =
                    gameplay::toEmitterParams(*form);
                params.duration = 1.0e9f; // for the fall's life
                // Spray scales with the fall: a 12 m cascade throws
                // more water than a spring wisp.
                const f32 boost =
                    glm::clamp(drop / 4.0f, 1.0f, 3.0f);
                params.rate *= boost;
                params.burst = static_cast<i32>(
                    static_cast<f32>(params.burst) * boost);
                params.shapeRadius =
                    glm::max(params.shapeRadius,
                             0.25f * fn.halfWidth);
                params.sizeStart *= 0.5f * (1.0f + boost);
                params.sizeEnd *= 0.5f * (1.0f + boost);
                emitters.haze = ctx.fxSim.spawn(params, at, seed);
            }
            if (sound.isValid()) {
                if (const auto params =
                        ctx.sounds.resolve(sound, at, seed)) {
                    emitters.sound = ctx.audio.play(*params);
                }
            }
            if (emitters.haze != 0 || emitters.sound != 0) {
                LOG_INFO("cascade fx: spray at ({:.0f}, {:.0f}) "
                         "drop {:.1f} m",
                         fn.x, fn.z, drop);
                riverCascadeEmitters.emplace(key, emitters);
            }
        }
    }
    sweep(seen);
}

void AmbientFxController::updateSources(const AmbientFxContext& ctx) {
    const Vec3 cam = ctx.cameraPosition;
    const auto within = [](const vector<GroundEvent>& events, const Vec3& at,
                           f32 extra) {
        for (const GroundEvent& e : events) {
            const f32 dx = at.x - e.at.x;
            const f32 dz = at.z - e.at.y;
            const f32 reach = e.radius + extra;
            if (dx * dx + dz * dz <= reach * reach) {
                return true;
            }
        }
        return false;
    };
    std::unordered_set<u64> seen;
    ctx.fxSourceQuery.each([&](flecs::entity e, const world::Transform& transform,
                               world::FxSource& source) {
        const Vec3 at = transform.position + transform.rotation * source.offset;
        const ecs::Entity entity { e };
        // The elements decide first: water, a strong wind or a downpour
        // put it out; fire (the ground alight under it, a spark or a
        // brand on it) lights it again.
        if (source.lit) {
            const bool water = source.douseByWater && within(waterEvents, at, 0.3f);
            const bool wind =
                source.snuffWind > 0.0f && ctx.policy.hasWeather &&
                glm::length(ctx.wind.windAt(at.x, at.z)) > source.snuffWind;
            const bool rain = source.snuffRain > 0.0f && ctx.policy.hasWeather &&
                              ctx.rainIntensity >= source.snuffRain;
            if (water || wind || rain) {
                snuffSource(ctx, entity, source, at);
            }
        } else if (ctx.spirits.fireBurningAt(at.x, at.z) ||
                   within(fireEvents, at, 0.3f)) {
            source.lit = true;
            if (entity.has<world::LightSource>()) {
                entity.get_mut<world::LightSource>().litScale = 1.0f;
            }
        }
        if (!source.lit || glm::distance(at, cam) > kSourceReach) {
            return; // out, or out of reach: swept below if it was alight
        }
        seen.insert(e.id());
        if (sourceEmitters.contains(e.id())) {
            return;
        }
        Emitters emitters;
        const u32 seed = static_cast<u32>(e.id() & 0xffffffffu) ^ 0x5eedf1a3u;
        if (const auto* form = ctx.forms.find<data::ParticleForm>(source.flame)) {
            fx::EmitterParams params = gameplay::toEmitterParams(*form);
            params.duration = 1.0e9f; // for the prop's life
            emitters.flame = ctx.fxSim.spawn(params, at, seed);
        }
        if (const auto* form = ctx.forms.find<data::ParticleForm>(source.smoke)) {
            fx::EmitterParams params = gameplay::toEmitterParams(*form);
            params.duration = 1.0e9f;
            emitters.smoke = ctx.fxSim.spawn(params, at, seed ^ 0x9e3779b9u);
        }
        if (const auto* form = ctx.forms.find<data::ParticleForm>(source.haze)) {
            fx::EmitterParams params = gameplay::toEmitterParams(*form);
            params.duration = 1.0e9f;
            emitters.haze = ctx.fxSim.spawn(params, at + Vec3 { 0.0f, 0.5f, 0.0f },
                                            seed ^ 0x7f4a7c15u);
        }
        if (source.sound.isValid()) {
            if (const auto params = ctx.sounds.resolve(source.sound, at, seed)) {
                emitters.sound = ctx.audio.play(*params);
            }
        }
        sourceEmitters.emplace(e.id(), emitters);
    });
    waterEvents.clear();
    fireEvents.clear();
    // Gone (despawned, disabled, snuffed) or out of reach: the emitters stop.
    for (auto it = sourceEmitters.begin(); it != sourceEmitters.end();) {
        if (seen.contains(it->first)) {
            ++it;
            continue;
        }
        for (const u32 emitter : { it->second.flame, it->second.smoke, it->second.haze }) {
            if (emitter != 0) {
                ctx.fxSim.stopEmitter(emitter);
            }
        }
        if (it->second.sound != 0) {
            ctx.audio.stop(it->second.sound, 0.5f);
        }
        it = sourceEmitters.erase(it);
    }
    // The igniters among the LIT sources in reach, then their heat.
    igniterList.clear();
    ctx.fxSourceQuery.each([&](flecs::entity e, const world::Transform& transform,
                               const world::FxSource& source) {
        if (source.igniteHeat <= 0.0f || !source.lit || !seen.contains(e.id())) {
            return;
        }
        const Vec3 at = transform.position + transform.rotation * source.offset;
        igniterList.push_back({ at, glm::max(source.igniteRadius, 0.3f),
                                source.igniteHeat, source.offset.y < 1.0f });
    });
}

void AmbientFxController::reset(const AmbientFxContext& ctx) {
    for (const auto& [id, emitters] : sourceEmitters) {
        for (const u32 emitter : { emitters.flame, emitters.smoke, emitters.haze }) {
            if (emitter != 0) {
                ctx.fxSim.stopEmitter(emitter);
            }
        }
        if (emitters.sound != 0) {
            ctx.audio.stop(emitters.sound, 0.3f);
        }
    }
    sourceEmitters.clear();
    igniterList.clear();
    waterEvents.clear();
    fireEvents.clear();
}

} // namespace game
