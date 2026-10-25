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

// The fire gestures: the flame jet, the brand, the ward, the fire reading.
// The field they feed and its presentation are the lane's and the
// FireDirector's (docs/FIRE-RENDER.md).

void SpiritDirector::igniteGround(const SpiritContext& ctx, f32 x, f32 z,
                                  f32 radius, f32 heat) {
    ignite(x, z, radius, heat);
    ctx.ambientFx.noteFireEvent({ x, z }, glm::max(radius, 0.5f));
}

void SpiritDirector::updateFlameJet(const SpiritContext& ctx, f32 dt) {
    if (!spiritFlameJet) {
        return;
    }
    SpiritFlameJet& jet = *spiritFlameJet;
    const bool held = ctx.castHeld;
    if (!held || !ctx.playerEntity.is_alive()) {
        endFlameJet(ctx);
        return;
    }
    jet.costClock += dt;
    if (jet.costClock >= jet.costPeriod) {
        jet.costClock -= jet.costPeriod;
        const auto* ability = ctx.forms.find<gameplay::AbilityForm>(jet.ability);
        auto& set = ctx.playerEntity.get_mut<gameplay::AttributeSet>();
        auto& system = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
        if (ability && !gameplay::payAbilityCost(*ability, set, system,
                                                 { ctx.forms, ctx.tags },
                                                 jet.upkeepScale)) {
            ctx.say(ctx.texts.get("spirit.refused"), 1.5f);
            endFlameJet(ctx);
            return;
        }
    }
    const Vec3 origin = nozzle(ctx);
    const Vec3 forward = ctx.cameraForward;
    if (jet.emitter != 0) {
        ctx.fxSim.moveEmitter(jet.emitter, origin);
        ctx.fxSim.steerEmitter(jet.emitter, forward * kFlameJetSpeed,
                           jet.range / kFlameJetSpeed);
    }
    // Whoever stands in the cone burns (the same contact as the field,
    // applied by the FireDirector's contact pass with the ground's fire).
    flameJetTouched.clear();
    if (ctx.policy.allowsSpirits) {
        const f32 cosHalf = std::cos(kFlameJetHalfAngle);
        for (const auto& npc : ctx.npcs) {
            if (!npc->entity.is_alive() || npc->dead ||
                !npc->entity.has<world::Transform>()) {
                continue;
            }
            const Vec3 to = npc->entity.get<world::Transform>().position +
                            Vec3 { 0.0f, 0.9f, 0.0f } - origin;
            const f32 dist = glm::length(to);
            const bool inCone = dist > 0.2f && dist <= jet.range &&
                                glm::dot(to / dist, forward) >= cosHalf;
            if (inCone) {
                flameJetTouched.insert(npc->entity.id());
            }
        }
    }
    // The ground under the cone catches, every pulse, out to the range
    // or the first hillside the flames hit.
    jet.pulseClock += dt;
    if (jet.pulseClock < kFlameJetPulse) {
        return;
    }
    jet.pulseClock -= kFlameJetPulse;
    const auto& params = ctx.renderer.terrainParams();
    for (f32 t = 1.5f; t <= jet.range; t += 1.5f) {
        const Vec3 p = origin + forward * t;
        const f32 ground = render::terrain::height(params, p.x, p.z);
        if (p.y < ground) {
            igniteGround(ctx, p.x, p.z, jet.radius, jet.heat); // the hillside
            break;
        }
        if (p.y - ground < 2.5f &&
            !render::terrain::waterSurfaceQuery(ctx.waterQuery(), p.x, p.z, ground)) {
            igniteGround(ctx, p.x, p.z, jet.radius, jet.heat);
        }
    }
}

void SpiritDirector::endFlameJet(const SpiritContext& ctx) {
    if (!spiritFlameJet) {
        return;
    }
    if (spiritFlameJet->emitter != 0) {
        ctx.fxSim.stopEmitter(spiritFlameJet->emitter);
    }
    spiritFlameJet.reset();
    flameJetTouched.clear();
}

void SpiritDirector::updateBrand(const SpiritContext& ctx, f32 dt) {
    if (!spiritBrand) {
        return;
    }
    SpiritBrand& brand = *spiritBrand;
    const bool held = ctx.castHeld;
    if (!held || !ctx.playerEntity.is_alive()) {
        endBrand(ctx);
        return;
    }
    brand.costClock += dt;
    if (brand.costClock >= brand.costPeriod) {
        brand.costClock -= brand.costPeriod;
        const auto* ability = ctx.forms.find<gameplay::AbilityForm>(brand.ability);
        auto& set = ctx.playerEntity.get_mut<gameplay::AttributeSet>();
        auto& system = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
        if (ability && !gameplay::payAbilityCost(*ability, set, system,
                                                 { ctx.forms, ctx.tags },
                                                 brand.upkeepScale)) {
            ctx.say(ctx.texts.get("spirit.refused"), 1.5f);
            endBrand(ctx);
            return;
        }
    }
    const std::optional<Vec3> aim = ctx.aimGround();
    if (!aim) {
        return;
    }
    if (brand.emitter != 0) {
        ctx.fxSim.moveEmitter(brand.emitter, *aim);
    }
    brand.pulseClock += dt;
    if (brand.pulseClock >= kBrandPulse) {
        brand.pulseClock -= kBrandPulse;
        // Never into water: the flame just hovers over it.
        if (!render::terrain::waterSurfaceQuery(ctx.waterQuery(), aim->x, aim->z, aim->y)) {
            igniteGround(ctx, aim->x, aim->z, brand.radius, brand.heat);
        }
    }
}

void SpiritDirector::endBrand(const SpiritContext& ctx) {
    if (!spiritBrand) {
        return;
    }
    if (spiritBrand->emitter != 0) {
        ctx.fxSim.stopEmitter(spiritBrand->emitter);
    }
    spiritBrand.reset();
}

void SpiritDirector::updateGlobe(const SpiritContext& ctx, f32 dt) {
    if (!spiritGlobe) {
        return;
    }
    SpiritGlobe& globe = *spiritGlobe;
    const bool held = ctx.castHeld;
    if (!held || !ctx.playerEntity.is_alive() || !ctx.playerBody) {
        endGlobe(ctx);
        return;
    }
    globe.costClock += dt;
    if (globe.costClock >= globe.costPeriod) {
        globe.costClock -= globe.costPeriod;
        const auto* ability = ctx.forms.find<gameplay::AbilityForm>(globe.ability);
        auto& set = ctx.playerEntity.get_mut<gameplay::AttributeSet>();
        auto& system = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
        if (ability && !gameplay::payAbilityCost(*ability, set, system,
                                                 { ctx.forms, ctx.tags },
                                                 globe.upkeepScale)) {
            ctx.say(ctx.texts.get("spirit.refused"), 1.5f);
            endGlobe(ctx);
            return;
        }
    }
    const Vec3 at = ctx.playerBody->position();
    setWard(world::FireDouse { at.x, at.z, globe.radius });
    if (globe.emitter != 0) {
        ctx.fxSim.moveEmitter(globe.emitter, at + Vec3 { 0.0f, 1.0f, 0.0f });
    }
}

void SpiritDirector::endGlobe(const SpiritContext& ctx) {
    if (!spiritGlobe) {
        return;
    }
    if (spiritGlobe->emitter != 0) {
        ctx.fxSim.stopEmitter(spiritGlobe->emitter);
    }
    spiritGlobe.reset();
    setWard(std::nullopt);
}

void SpiritDirector::castFireReading(const SpiritContext& ctx, const Vec3& at,
                                     f32 seconds, bool live) {
    const auto num = [](f32 v, i32 decimals) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), decimals == 0 ? "%.0f" : "%.1f",
                      static_cast<f64>(v));
        return str { buf };
    };
    vector<str> lines;
    lines.push_back(ctx.texts.get("fire.title"));
    // The cell: what the last landed job published.
    const f32 glow = fireGlowAt(at.x, at.z);
    const f32 scorch = fireScorchAt(at.x, at.z);
    if (glow >= 0.25f) {
        lines.push_back(ctx.texts.format("fire.burning", { num(glow * 100.0f, 0) }));
    } else if (scorch > 0.05f) {
        lines.push_back(ctx.texts.format("fire.burnt", { num((1.0f - scorch) * 100.0f, 0) }));
    } else {
        lines.push_back(ctx.texts.get("fire.cold"));
    }
    // The fuel the ground holds (the same blend the kernel samples).
    const auto& params = ctx.renderer.terrainParams();
    const f32 h = render::terrain::height(params, at.x, at.z);
    const Vec3 n = render::terrain::normal(params, at.x, at.z);
    const render::terrain::RegionFields fields =
        render::terrain::regionFieldsAt(params, at.x, at.z);
    const render::terrain::MaterialWeights w =
        render::terrain::materialWeightsAt(params, at.x, at.z, h, n, fields);
    const render::terrain::FireCellFuel fuel = world::fuelFromWeights(
        groundFuel(),
        { w.grass, w.rock, w.cliff, w.snow, w.sand }, fields.wetness);
    lines.push_back(ctx.texts.format("fire.fuel",
                                 { num(fuel.fuel, 0), num(fuel.flammability * 100.0f, 0),
                                   num(fuel.moisture * 100.0f, 0) }));
    // The nearest front.
    f32 best = 1.0e9f;
    Vec2 front { 0.0f };
    for (const Vec2& cell : fireBurning()) {
        const Vec2 to { cell.x - at.x, cell.y - at.z };
        const f32 d2 = glm::dot(to, to);
        if (d2 < best) {
            best = d2;
            front = to;
        }
    }
    if (best < 1.0e9f && best > 4.0f) {
        lines.push_back(ctx.texts.format(
            "fire.front", { num(std::sqrt(best), 0), ctx.texts.get(str { "dir." } + world::compassCode(front)) }));
    } else if (best <= 4.0f) {
        lines.push_back(ctx.texts.get("fire.frontHere"));
    } else {
        lines.push_back(ctx.texts.get("fire.noFront"));
    }
    // The wind the front will follow (the field at the aim; the
    // reading's compass, like the front's bearing).
    const Vec2 wind = worldWind.windAt(at.x, at.z);
    const f32 windSpeed = glm::length(wind);
    if (windSpeed < 0.3f) {
        lines.push_back(ctx.texts.get("fire.stillAir"));
    } else {
        lines.push_back(ctx.texts.format(
            "fire.wind", { num(windSpeed, 0), ctx.texts.get(str { "dir." } + world::compassCode(wind)) }));
    }
    const bool opening = !spiritReadingLive && spiritReadingSeconds <= 0.0f;
    spiritReadingKind = ReadingKind::Fire;
    spiritReading = std::move(lines);
    spiritReading.resize(kReadingLines);
    spiritReadingLive = live;
    spiritReadingSeconds = live ? 0.0f : glm::max(seconds, 1.0f);
    if (opening) {
        ctx.cues.emit({ "Cue.Spirit.Fire.Spawn", at, 0.3f });
    }
}

} // namespace game
