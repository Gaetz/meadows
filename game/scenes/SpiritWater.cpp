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

// The water gestures: the jets, the carried volume, the water reading
// (docs/SPELLS.md).

void SpiritDirector::updateJets(const SpiritContext& ctx, f32 dt) {
    if (jets.empty()) {
        return;
    }
    // Channeled jets live while the key is held and re-pay their
    // ability's cost every costPeriod (§2.9: through the cost effect);
    // release or an unaffordable upkeep ends them (the tick expires
    // remaining == 0 and hands the emitter back).
    const bool held = ctx.castHeld;
    for (world::SpiritJet& jet : jets.entriesMut()) {
        if (!jet.channeled || jet.remaining <= 0.0f) {
            continue;
        }
        if (!held || !ctx.playerEntity.is_alive()) {
            jet.remaining = 0.0f;
            continue;
        }
        jet.costClock += dt;
        if (jet.costClock < jet.costPeriod) {
            continue;
        }
        jet.costClock -= jet.costPeriod;
        const auto* ability = ctx.forms.find<gameplay::AbilityForm>(jet.ability);
        if (!ability) {
            continue;
        }
        auto& set = ctx.playerEntity.get_mut<gameplay::AttributeSet>();
        auto& system = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
        if (!gameplay::payAbilityCost(*ability, set, system,
                                      { ctx.forms, ctx.tags }, jet.upkeepScale)) {
            jet.remaining = 0.0f;
            ctx.say(ctx.texts.get("spirit.refused"), 1.5f);
        }
    }
    jets.aim(nozzle(ctx), ctx.cameraForward);
    const auto& params = ctx.terrainParams;
    jets.resolveLandings([&params](f32 x, f32 z) {
        return render::terrain::height(params, x, z);
    });
    for (const world::SpiritJet& jet : jets.entries()) {
        if (jet.emitter != 0) {
            ctx.fxSim.moveEmitter(jet.emitter, jet.origin);
            // Particles die where the arc lands: the stream ends at the
            // ground instead of sinking through it.
            ctx.fxSim.steerEmitter(jet.emitter, jet.velocity,
                               jet.landing ? jet.landing->flightSeconds
                                           : 2.0f);
        }
    }
    // The stream's lumps: every landing is a splash where THAT lump
    // fell — the arrival reads as water hitting ground, not as a rate.
    vector<Vec3> landed;
    jets.advanceSpheres(dt, kJetSphereInterval, world::SpiritJetList::kGravity,
                        &landed);
    for (const Vec3& at : landed) {
        ctx.cues.emit({ "Cue.Spirit.Water.Jet", at, 1.0f });
        ctx.ambientFx.noteWaterEvent({ at.x, at.z }, 1.5f);
    }
    // A lump flying through an actor shoves him along its flight (the
    // shove rides Npc::shove, applied and decayed by the NPC director).
    if (ctx.policy.allowsSpirits) {
        for (const world::SpiritJet& jet : jets.entries()) {
            if (jet.kind != render::terrain::SpiritKind::Water) {
                continue;
            }
            for (const world::JetSphere& sphere : jet.spheres) {
                const Vec3 at = sphere.at(world::SpiritJetList::kGravity);
                const Vec2 flat { sphere.velocity.x, sphere.velocity.z };
                const f32 speed = glm::length(flat);
                if (speed < 1e-3f) {
                    continue;
                }
                const f32 reach = sphere.radius + 0.6f;
                ctx.ambientFx.noteWaterEvent({ at.x, at.z }, reach); // snuffs a torch it crosses
                for (const auto& npc : ctx.npcs) {
                    if (!npc->entity.is_alive() || npc->dead ||
                        !npc->entity.has<world::Transform>()) {
                        continue;
                    }
                    const Vec3 chest = npc->entity.get<world::Transform>().position +
                                       Vec3 { 0.0f, 0.9f, 0.0f };
                    if (glm::distance(chest, at) > reach) {
                        continue;
                    }
                    npc->shove += flat / speed * (kWaterJetShoveRate * dt);
                    const f32 shove = glm::length(npc->shove);
                    if (shove > kWaterJetShoveMax) {
                        npc->shove *= kWaterJetShoveMax / shove;
                    }
                }
            }
        }
    }
    // The landing spots move with the aim: the kernel gets them fresh
    // every frame (a value copy of a handful of discs).
    ctx.pushWaterSources();
}

void SpiritDirector::updateHold(const SpiritContext& ctx, f32 dt) {
    if (!spiritHold) {
        return;
    }
    world::SpiritHold& hold = *spiritHold;
    const bool held = ctx.castHeld;
    if (!held || !ctx.playerEntity.is_alive()) {
        releaseHold(ctx, true); // dropped where the aim is
        return;
    }
    // Upkeep: the ability's cost again every costPeriod (§2.9).
    hold.costClock += dt;
    if (hold.costClock >= hold.costPeriod) {
        hold.costClock -= hold.costPeriod;
        const auto* ability = ctx.forms.find<gameplay::AbilityForm>(hold.ability);
        auto& set = ctx.playerEntity.get_mut<gameplay::AttributeSet>();
        auto& system = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
        if (ability && !gameplay::payAbilityCost(*ability, set, system,
                                                 { ctx.forms, ctx.tags },
                                                 hold.upkeepScale)) {
            ctx.say(ctx.texts.get("spirit.refused"), 1.5f);
            releaseHold(ctx, true);
            return;
        }
    }
    // Draw where the aim rests: water present there = the hold fills
    // and a draining source works the kernel (the pin lets go under it).
    hold.aim = ctx.aimGround();
    std::optional<render::terraingen::WaterSource> draw;
    if (hold.aim) {
        const std::optional<f32> surface = render::terrain::waterSurfaceQuery(
            ctx.waterQuery(), hold.aim->x, hold.aim->z, hold.aim->y);
        const bool present = surface && *surface - hold.aim->y > 0.02f;
        if (hold.absorb(dt, present) && present) {
            draw = render::terraingen::WaterSource { hold.aim->x, hold.aim->z,
                                                     -hold.rate };
        }
        if (hold.emitter != 0) {
            ctx.fxSim.moveEmitter(hold.emitter,
                              *hold.aim + Vec3 { 0.0f, 2.5f, 0.0f });
        }
    }
    setHoldSource(draw);
    ctx.pushWaterSources();
}

void SpiritDirector::releaseHold(const SpiritContext& ctx, bool drop) {
    if (!spiritHold) {
        return;
    }
    world::SpiritHold hold = *spiritHold;
    spiritHold.reset();
    if (hold.emitter != 0) {
        ctx.fxSim.stopEmitter(hold.emitter);
    }
    setHoldSource(std::nullopt);
    // The carried volume falls where the aim is: a one-second source
    // (the same placed-source path, cue included).
    constexpr f32 kDropSeconds = 1.0f;
    if (drop && hold.volume > 0.0f && hold.aim) {
        PendingAction action;
        action.kind = hold.kind;
        action.x = hold.aim->x;
        action.z = hold.aim->z;
        action.rate = hold.dropDischarge(kDropSeconds);
        action.radius = hold.radius;
        action.seconds = kDropSeconds;
        pendingActions.push_back(action);
    }
    ctx.pushWaterSources();
}

void SpiritDirector::castWaterReading(const SpiritContext& ctx, const Vec3& at,
                                      f32 seconds, bool live) {
    const render::terrain::WaterQuery query = ctx.waterQuery();
    world::WaterReadingInputs in;
    in.query = &query;
    in.sources = &list();
    in.worldspace = ctx.worldspace;
    const auto& params = ctx.terrainParams;
    in.ground = [&params](f32 x, f32 z) {
        return render::terrain::height(params, x, z);
    };
    const world::WaterReading r = world::readWater(in, at.x, at.z, at.y);

    const auto num = [](f32 v, i32 decimals) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), decimals == 0 ? "%.0f" : "%.1f",
                      static_cast<f64>(v));
        return str { buf };
    };
    const auto dir = [&](const Vec2& d) {
        return ctx.texts.get(str { "dir." } + world::compassCode(d));
    };
    vector<str> lines;
    lines.push_back(ctx.texts.get("reading.title"));
    if (!r.water) {
        lines.push_back(ctx.texts.get("reading.dry"));
        if (r.nearestFound) {
            lines.push_back(ctx.texts.format(
                "reading.nearest",
                { num(r.nearestDistance, 0), dir(r.nearestDir) }));
        } else {
            lines.push_back(
                ctx.texts.format("reading.noNearest", num(in.searchRadius, 0)));
        }
    } else {
        switch (r.body) {
        case world::WaterReading::Body::Sea:
            lines.push_back(ctx.texts.get("reading.body.sea"));
            break;
        case world::WaterReading::Body::Lake:
            lines.push_back(ctx.texts.format(
                "reading.body.lake",
                { num(r.lakeLevel, 0), num(r.lakeArea / 10000.0f, 1) }));
            break;
        case world::WaterReading::Body::River:
            lines.push_back(ctx.texts.format(
                "reading.body.river",
                { num(r.riverWidth, 0), num(r.riverDischarge, 1) }));
            break;
        case world::WaterReading::Body::Spirit:
            lines.push_back(r.spiritRemaining < 0.0f
                                ? ctx.texts.get("reading.body.spiritPermanent")
                                : ctx.texts.format("reading.body.spirit",
                                               num(r.spiritRemaining, 0)));
            break;
        case world::WaterReading::Body::Pool:
        case world::WaterReading::Body::None:
            lines.push_back(ctx.texts.get("reading.body.pool"));
            break;
        }
        lines.push_back(ctx.texts.format("reading.depth", num(r.depth, 1)));
        const f32 speed = glm::length(r.flow);
        if (speed > 0.05f) {
            lines.push_back(ctx.texts.format("reading.flow",
                                         { num(speed, 1), dir(r.flow) }));
        } else {
            lines.push_back(ctx.texts.get("reading.still"));
        }
        if (r.volumeKnown) {
            lines.push_back(ctx.texts.format(
                r.volumeExact ? "reading.volume" : "reading.volumeApprox",
                num(r.volume, 0)));
        }
    }
    // The cue only when the reading opens, not on every live refresh.
    const bool opening = !spiritReadingLive && spiritReadingSeconds <= 0.0f;
    spiritReadingKind = ReadingKind::Water;
    spiritReading = std::move(lines);
    spiritReading.resize(kReadingLines);
    spiritReadingLive = live;
    spiritReadingSeconds = live ? 0.0f : glm::max(seconds, 1.0f);
    if (opening) {
        ctx.cues.emit({ "Cue.Spirit.Water.Read", at, 1.0f });
    }
}

} // namespace game
