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

// The wind gestures: the field the world reads, the breath, the calmed
// and steered weather wind (docs/CHANTIER-ESPRITS.md).

void SpiritDirector::updateWind(const SpiritContext& ctx, f32 dt) {
    // The field this frame: the weather's global wind + the placed gusts
    // (Wind sources, in SIM seconds like every source) + the breath.
    const Vec2 dir = render::terrain::windDirectionFromDegrees(ctx.atmos.windDirectionDeg);
    worldWind.globalDir = dir;
    worldWind.globalSpeed = glm::max(ctx.atmos.windStrength, 0.0f) * kWindSpeedPerStrength;
    worldWind.gusts.clear();
    const core::Guid here = ctx.worldspace;
    for (const world::SpiritSourceList::Entry& entry : list().entries()) {
        const render::terrain::SpiritSource& source = entry.source;
        if (source.kind != render::terrain::SpiritKind::Wind || entry.worldspace != here) {
            continue;
        }
        worldWind.gusts.push_back({ source.x, source.z, source.dirX, source.dirZ,
                                    source.rate, glm::max(source.radius, 1.0f),
                                    source.remaining < 0.0f ? 1.0e9f : source.remaining });
    }
    if (spiritBlow) {
        const Vec3 forward = ctx.cameraForward;
        const Vec3 at = nozzle(ctx) + forward * (spiritBlow->range * 0.5f);
        const Vec2 flat = glm::length(Vec2 { forward.x, forward.z }) > 1e-3f
                              ? glm::normalize(Vec2 { forward.x, forward.z })
                              : Vec2 { 1.0f, 0.0f };
        worldWind.gusts.push_back({ at.x, at.z, flat.x, flat.y, spiritBlow->speed,
                                    glm::max(spiritBlow->range * 0.5f, spiritBlow->radius),
                                    1.0f });
    }
    // Consumers: the particles sample it per particle, the player's body
    // is pushed by the GUSTS (never by the ambient wind).
    ctx.fxSim.setWindSampler([this](const Vec3& p) {
        const Vec2 w = worldWind.windAt(p.x, p.z);
        return Vec3 { w.x, 0.0f, w.y };
    });
    if (phys::CharacterBody* body = ctx.playerBody) {
        const Vec3 feet = body->position();
        const Vec2 gusts = worldWind.windAt(feet.x, feet.z) -
                           worldWind.globalDir * worldWind.globalSpeed;
        body->setExternalVelocity(Vec3 { gusts.x, 0.0f, gusts.y } * kPlayerWindPush);
    }
    (void)dt;
}

void SpiritDirector::updateBlow(const SpiritContext& ctx, f32 dt) {
    if (!spiritBlow) {
        return;
    }
    SpiritBlow& blow = *spiritBlow;
    const bool held = ctx.castHeld;
    if (!held || !ctx.playerEntity.is_alive()) {
        endBlow(ctx);
        return;
    }
    blow.costClock += dt;
    if (blow.costClock >= blow.costPeriod) {
        blow.costClock -= blow.costPeriod;
        const auto* ability = ctx.forms.find<gameplay::AbilityForm>(blow.ability);
        auto& set = ctx.playerEntity.get_mut<gameplay::AttributeSet>();
        auto& system = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
        if (ability && !gameplay::payAbilityCost(*ability, set, system,
                                                 { ctx.forms, ctx.tags },
                                                 blow.upkeepScale)) {
            ctx.say(ctx.texts.get("spirit.refused"), 1.5f);
            endBlow(ctx);
            return;
        }
    }
    if (blow.emitter != 0) {
        const Vec3 forward = ctx.cameraForward;
        ctx.fxSim.moveEmitter(blow.emitter, nozzle(ctx));
        ctx.fxSim.steerEmitter(blow.emitter, forward * blow.speed,
                           blow.range / glm::max(blow.speed, 1.0f));
    }
}

void SpiritDirector::endBlow(const SpiritContext& ctx) {
    if (!spiritBlow) {
        return;
    }
    if (spiritBlow->emitter != 0) {
        ctx.fxSim.stopEmitter(spiritBlow->emitter);
    }
    spiritBlow.reset();
}

void SpiritDirector::updateWindHold(const SpiritContext& ctx, f32 dt) {
    if (!spiritWindHold) {
        return;
    }
    SpiritWindHold& hold = *spiritWindHold;
    // The base: what the weather sets. Refreshed while a crossfade runs
    // (it rewrites the atmosphere each frame ahead of this).
    if (ctx.weatherTransitioning) {
        windBaseStrength = ctx.atmos.windStrength;
        windBaseDirectionDeg = ctx.atmos.windDirectionDeg;
    }
    const bool held = ctx.castHeld;
    if (!held || !ctx.playerEntity.is_alive()) {
        hold.releasing = true; // the wind eases back before the hold ends
    }
    hold.costClock += hold.releasing ? 0.0f : dt;
    if (!hold.releasing && hold.costClock >= hold.costPeriod) {
        hold.costClock -= hold.costPeriod;
        const auto* ability = ctx.forms.find<gameplay::AbilityForm>(hold.ability);
        auto& set = ctx.playerEntity.get_mut<gameplay::AttributeSet>();
        auto& system = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
        if (ability && !gameplay::payAbilityCost(*ability, set, system,
                                                 { ctx.forms, ctx.tags },
                                                 hold.upkeepScale)) {
            ctx.say(ctx.texts.get("spirit.refused"), 1.5f);
            endWindHold(ctx);
            return;
        }
    }
    // Both effects ease toward their target (the spell's while held, the
    // weather's while releasing): the wind turns and dies over about a
    // second, never snaps.
    const f32 ease = 1.0f - std::exp(-dt / kWindHoldEase);
    if (hold.calm) {
        const f32 target = hold.releasing ? 1.0f : 0.0f;
        hold.calmFactor += (target - hold.calmFactor) * ease;
        ctx.atmos.windStrength = windBaseStrength * hold.calmFactor;
    }
    if (hold.direct) {
        f32 targetDeg = windBaseDirectionDeg;
        const Vec3 forward = ctx.cameraForward;
        if (!hold.releasing && glm::length(Vec2 { forward.x, forward.z }) > 1e-3f) {
            // The compass of windDirectionFromDegrees: dir = (cos, -sin).
            targetDeg = glm::degrees(std::atan2(-forward.z, forward.x));
        }
        // Along the short arc (350 -> 10 turns +20, never -340).
        const f32 delta =
            std::fmod(targetDeg - hold.currentDeg + 540.0f, 360.0f) - 180.0f;
        hold.currentDeg += delta * ease;
        ctx.atmos.windDirectionDeg = hold.currentDeg;
    }
    if (hold.releasing) {
        const bool calmDone = !hold.calm || hold.calmFactor > 0.99f;
        const f32 left =
            std::fmod(windBaseDirectionDeg - hold.currentDeg + 540.0f, 360.0f) - 180.0f;
        const bool directDone = !hold.direct || std::abs(left) < 0.5f;
        if (calmDone && directDone) {
            endWindHold(ctx); // snaps the last fraction of a degree
        }
    }
}

void SpiritDirector::endWindHold(const SpiritContext& ctx) {
    if (!spiritWindHold) {
        return;
    }
    ctx.atmos.windStrength = windBaseStrength;
    ctx.atmos.windDirectionDeg = windBaseDirectionDeg;
    spiritWindHold.reset();
}

void SpiritDirector::resetWind(const SpiritContext& ctx) {
    endBlow(ctx);
    endWindHold(ctx);
    worldWind.gusts.clear();
    ctx.fxSim.setWindSampler({});
    if (phys::CharacterBody* body = ctx.playerBody) {
        body->setExternalVelocity(Vec3 { 0.0f });
    }
}

} // namespace game
