#include "game/scenes/SpiritDirector.hpp"
#include "world/worldspace/WorldForms.hpp"
#include "data/plugins/RecordDiff.hpp"

#include <algorithm>
#include <cmath>

#include "data/forms/FormQuery.hpp"
#include "data/forms/LocForms.hpp"    // TextTable
#include "data/forms/SpiritForms.hpp" // SpiritForm, SpellForm
#include "data/forms/VisualForms.hpp" // ParticleForm
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "engine/fx/Particles.hpp"
#include "engine/physics/Physics.hpp"
#include "engine/render/SceneView.hpp" // RenderSnapshot
#include "engine/render/WorldRenderer.hpp"
#include "engine/terrain/WaterQuery.hpp"
#include "game/scenes/AmbientFxController.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/cue/GameplayCues.hpp" // toEmitterParams
#include "world/spirit/SpiritWaterMesh.hpp"

namespace game {

void SpiritDirector::build(const data::FormDatabase& forms) {
    table = world::compileSpiritRules(forms);
    for (const str& error : table.errors) {
        LOG_WARN("Spirits: {}", error);
    }
    groundProps = world::groundPropsFrom(table);
    jetFx.fill(core::Guid {});
    holdFx.fill(core::Guid {});
    fieldFx.fill(core::Guid {});
    sparkFx.fill(core::Guid {});
    soundFx.fill(core::Guid {});
    smokeFx.fill(core::Guid {});
    hazeFx.fill(core::Guid {});
    contactFx.fill(core::Guid {});
    contactEvery.fill(0.5f);
    contactHurt.fill(0.0f);
    data::forEach<data::SpiritForm>(forms, [&](const data::SpiritForm& spirit) {
        const auto kind = render::terrain::spiritFromName(spirit.name);
        if (kind != render::terrain::SpiritKind::kCount) {
            jetFx[static_cast<size_t>(kind)] = spirit.jetParticles;
            holdFx[static_cast<size_t>(kind)] = spirit.holdParticles;
            fieldFx[static_cast<size_t>(kind)] = spirit.fieldParticles;
            sparkFx[static_cast<size_t>(kind)] = spirit.emberParticles;
            soundFx[static_cast<size_t>(kind)] = spirit.fieldSound;
            smokeFx[static_cast<size_t>(kind)] = spirit.smokeParticles;
            hazeFx[static_cast<size_t>(kind)] = spirit.hazeParticles;
            contactFx[static_cast<size_t>(kind)] = spirit.contactEffect;
            contactEvery[static_cast<size_t>(kind)] =
                glm::max(spirit.contactPeriod, 0.05f);
            contactHurt[static_cast<size_t>(kind)] = spirit.contactDamage;
        }
        if (kind == render::terrain::SpiritKind::Earth) {
            lift.quadratic = spirit.liftQuadratic;
            lift.min = spirit.liftMin;
            lift.max = spirit.liftMax;
        }
        if (kind == render::terrain::SpiritKind::Fire) {
            fireParams.spreadRate = spirit.spreadRate;
            fireParams.calmSpread = glm::max(spirit.fireCalmSpread, 0.0f);
            fireParams.windSpread = glm::max(spirit.fireWindSpread, 0.0f);
            fireParams.upwindSpread = glm::max(spirit.fireUpwindSpread, 0.0f);
            fireParams.ignitionPoints = glm::max(spirit.ignitionPoints, 0.01f);
            fireParams.spreadBudgetPerTick =
                static_cast<u32>(glm::max(spirit.spreadBudgetPerTick, 1));
            if (spirit.decayPerSecond > 0.0f) {
                fireParams.heatDecay = spirit.decayPerSecond;
            }
            fireParams.emberSeconds = glm::max(spirit.emberSeconds, 0.01f);
            fireParams.regrowSeconds = glm::max(spirit.regrowSeconds, 0.01f);
            treeFireParams.ignitionSeconds = glm::max(spirit.treeIgnitionSeconds, 0.1f);
            treeFireParams.burnSeconds = glm::max(spirit.treeBurnSeconds, 0.1f);
            treeFireParams.regrowSeconds = glm::max(spirit.treeRegrowSeconds, 1.0f);
        }
    });
    holdSource.reset();
    sources.apply(forms);
    if (!sources.entries().empty()) {
        LOG_INFO("Spirits: {} source(s) placed from the records",
                 sources.entries().size());
    }
}

core::Guid SpiritDirector::spawn(render::terrain::SpiritKind kind, f32 x,
                                 f32 z, f32 rate, f32 radius,
                                 f32 durationSimSeconds,
                                 const core::Guid& worldspace, f32 dirX,
                                 f32 dirZ) {
    render::terrain::SpiritSource source;
    source.kind = kind;
    source.x = x;
    source.z = z;
    source.dirX = dirX;
    source.dirZ = dirZ;
    source.rate = rate;
    source.radius = radius;
    source.remaining = durationSimSeconds;
    return sources.add(source, worldspace);
}

// --- The fire lane ---------------------------------------------------------

f32 SpiritDirector::fireScorchAt(f32 x, f32 z) const {
    const render::terraingen::GridSpec& spec = fireMaskSpec;
    if (spec.n == 0 || fireMask.size() < spec.cells()) {
        return 0.0f;
    }
    const i32 n = static_cast<i32>(spec.n);
    const i32 col = static_cast<i32>(std::lround((x - spec.originX) / spec.texelSize));
    const i32 row = static_cast<i32>(std::lround((z - spec.originZ) / spec.texelSize));
    if (col < 0 || row < 0 || col >= n || row >= n) {
        return 0.0f;
    }
    return static_cast<f32>(fireMask[static_cast<size_t>(row) * n + col]) / 255.0f;
}

bool SpiritDirector::fireBurningAt(f32 x, f32 z) const {
    const render::terraingen::GridSpec& spec = fireMaskSpec;
    if (spec.n == 0 || fireStateMask.size() < spec.cells()) {
        return false;
    }
    const i32 n = static_cast<i32>(spec.n);
    const i32 col = static_cast<i32>(std::lround((x - spec.originX) / spec.texelSize));
    const i32 row = static_cast<i32>(std::lround((z - spec.originZ) / spec.texelSize));
    if (col < 0 || row < 0 || col >= n || row >= n) {
        return false;
    }
    return fireStateMask[static_cast<size_t>(row) * n + col] ==
           static_cast<u8>(render::terrain::FireState::Burning);
}

f32 SpiritDirector::fireGlowAt(f32 x, f32 z) const {
    const render::terraingen::GridSpec& spec = fireMaskSpec;
    if (spec.n == 0 || fireGlowMask.size() < spec.cells()) {
        return 0.0f;
    }
    const i32 n = static_cast<i32>(spec.n);
    const i32 col = static_cast<i32>(std::lround((x - spec.originX) / spec.texelSize));
    const i32 row = static_cast<i32>(std::lround((z - spec.originZ) / spec.texelSize));
    if (col < 0 || row < 0 || col >= n || row >= n) {
        return 0.0f;
    }
    return static_cast<f32>(fireGlowMask[static_cast<size_t>(row) * n + col]) /
           255.0f;
}

void SpiritDirector::ignite(f32 x, f32 z, f32 radius, f32 heat) {
    if (heat <= 0.0f) {
        return;
    }
    fireIgnitions.push_back({ x, z, glm::max(radius, 0.5f), heat });
}

void SpiritDirector::douse(f32 x, f32 z, f32 radius) {
    fireDouses.push_back({ x, z, glm::max(radius, 0.5f) });
}

void SpiritDirector::resetFire() {
    ++fireEpoch; // an in-flight job lands stale and is dropped
    fireGrid.reset();
    fireIgnitions.clear();
    fireDouses.clear();
    fireWard.reset();
    fireActive = false;
    fireWake = false;
    fireAccum = 0.0f;
    fireMask.clear();
    fireGlowMask.clear();
    fireStateMask.clear();
    fireMaskSpec = {};
    fireCenters.clear();
    fireCells.clear();
    fireRestores.clear();
    lastFireStats = {};
}

vector<data::Record> SpiritDirector::capture(const core::Guid& worldspace) const {
    vector<data::Record> records = sources.capture();
    if (fireCells.empty()) {
        return records;
    }
    world::FireStateForm form;
    form.worldspace = worldspace;
    form.cells = world::packFireCells(fireCells);
    form.cellCount = static_cast<i32>(glm::min<size_t>(fireCells.size(), 65536));
    static const world::FireStateForm kDefaults {};
    const reflect::TypeInfo& type = world::FireStateForm::staticTypeInfo();
    data::Record record;
    record.formId = *core::Guid::fromString("5a5e0000-0000-4000-8000-0000000000f1");
    record.typeId = type.id;
    record.creates = true;
    data::diffToRecord(type, &form, &kDefaults, record,
                       /*includeInherited=*/false);
    records.push_back(std::move(record));
    return records;
}

void SpiritDirector::restoreFire(const world::FireStateForm& form) {
    fireRestores = world::unpackFireCells(form.cells);
    if (!fireRestores.empty()) {
        LOG_INFO("Fire: {} cell(s) restored from the save", fireRestores.size());
    }
}

bool SpiritDirector::updateFire(core::JobSystem& jobs, const FireFrame& frame,
                                f32 simSeconds) {
    using render::terrain::FireGrid;
    bool landed = false;
    world::FireJobOutput out;
    while (fireShared->done.tryPop(out)) {
        fireInFlight = false;
        if (out.epoch != fireEpoch) {
            continue; // the map changed under it
        }
        const bool wasActive = fireActive;
        fireGrid = std::make_unique<FireGrid>(std::move(out.grid));
        fireMask = std::move(out.scorch);
        fireGlowMask = std::move(out.glow);
        fireStateMask = std::move(out.state);
        fireMaskSpec = fireGrid->spec;
        fireCenters = std::move(out.burning);
        fireCells = std::move(out.cells);
        lastFireStats = out.stats;
        fireMs = out.millis;
        fireActive = out.active;
        landed = true;
        if (wasActive && !fireActive) {
            LOG_INFO("Fire: out — {} cell(s) burnt", lastFireStats.burnt);
        }
    }
    if (fireActive) {
        fireAccum += glm::max(simSeconds, 0.0f);
    }
    if (fireInFlight || !frame.params) {
        return landed;
    }
    const bool sparks = !fireIgnitions.empty() || !fireRestores.empty() ||
                        fireWake ||
                        (fireActive && (!fireDouses.empty() || fireWard));
    fireWake = false;
    if (!sparks && !fireActive) {
        fireDouses.clear(); // nothing burns: nothing to put out
        return landed; // the lane idles: no job while nothing burns
    }
    u32 steps = static_cast<u32>(std::floor(fireAccum / fireParams.dt));
    steps = glm::min(steps, kMaxStepsPerJob);
    if (steps == 0) {
        if (!sparks) {
            return landed; // not due yet
        }
        steps = 1; // a spark lands now, whatever the clock says
    }
    fireAccum = glm::max(0.0f, fireAccum - static_cast<f32>(steps) * fireParams.dt);

    world::FireJobInput in;
    if (fireGrid) {
        in.grid = std::move(*fireGrid);
        fireGrid.reset();
        i32 dCol = 0, dRow = 0;
        world::FireWindow::scrollFor(in.grid.spec, frame.focus.x, frame.focus.y,
                                     &dCol, &dRow);
        in.spec = (dCol != 0 || dRow != 0)
                      ? world::FireWindow::specFor(frame.focus.x, frame.focus.y)
                      : in.grid.spec;
    } else {
        in.spec = world::FireWindow::specFor(frame.focus.x, frame.focus.y);
    }
    in.params = fireParams;
    in.params.rain = glm::clamp(frame.rain, 0.0f, 1.0f);
    in.restores = std::move(fireRestores);
    fireRestores.clear();
    in.wind = laneWind;
    in.windFullSpeed = kWindFullSpeed;
    in.ignitions = std::move(fireIgnitions);
    fireIgnitions.clear();
    in.douses = std::move(fireDouses);
    fireDouses.clear();
    if (fireWard) {
        in.douses.push_back(*fireWard);
    }
    in.steps = steps;
    in.epoch = fireEpoch;
    // Value copies for the worker: the terrain (one shared copy for both
    // samplers), the material props, the water snapshot and bodies.
    auto params = std::make_shared<const render::TerrainParams>(*frame.params);
    const world::GroundProps props = groundProps;
    in.fuel = [params, props](f32 x, f32 z) {
        const f32 h = render::terrain::height(*params, x, z);
        const Vec3 n = render::terrain::normal(*params, x, z);
        const render::terrain::RegionFields fields =
            render::terrain::regionFieldsAt(*params, x, z);
        const render::terrain::MaterialWeights w =
            render::terrain::materialWeightsAt(*params, x, z, h, n, fields);
        return world::fuelFromWeights(
            props, { w.grass, w.rock, w.cliff, w.snow, w.sand }, fields.wetness);
    };
    const sptr<const render::terrain::WaterSimSnapshot> water = frame.water;
    const sptr<const render::WaterBodies> bodies = frame.bodies;
    const f32 seaLevel = frame.seaLevel;
    in.wet = [params, water, bodies, seaLevel](f32 x, f32 z) {
        const render::terrain::WaterQuery q { water.get(), bodies.get(),
                                              seaLevel };
        const f32 ground = render::terrain::height(*params, x, z);
        const std::optional<f32> surface =
            render::terrain::waterSurfaceQuery(q, x, z, ground);
        return surface && *surface - ground > 0.03f;
    };
    fireInFlight = true;
    jobs.enqueue([shared = fireShared, input = std::move(in)]() mutable {
        shared->done.push(world::runFireJob(std::move(input)));
    });
    return landed;
}

// --- The gestures (docs/SPELLS.md) -----------------------------------------

void SpiritDirector::stopEmitters(fx::ParticleSim& fxSim,
                                  const vector<u32>& emitters) {
    for (const u32 id : emitters) {
        if (id != 0) {
            fxSim.stopEmitter(id);
        }
    }
}

Vec3 SpiritDirector::nozzle(const SpiritContext& ctx) {
    // Below and ahead of the eye: the stream leaves a hand, not the
    // lens, so it reads in first person without covering the view.
    return ctx.cameraPosition + ctx.cameraForward * 0.45f +
           Vec3 { 0.0f, -0.35f, 0.0f };
}

void SpiritDirector::cast(const SpiritContext& ctx) {
    if (!ctx.playMode || !spiritAbility ||
        !ctx.playerEntity.is_alive() ||
        !ctx.playerEntity.has<gameplay::AbilitySystem>()) {
        return;
    }
    // The ability's spell (docs/SPELLS.md): its first SpellForm child.
    // No child = a scripted ability (the escape hatch).
    const data::SpellForm* spellForm = nullptr;
    data::childrenOf<data::SpellForm>(
        ctx.forms, spiritAbility->id, [&](const data::SpellForm& s) {
            if (!spellForm) {
                spellForm = &s;
            }
        });
    std::optional<world::SpellSpec> spell;
    if (spellForm) {
        str error;
        spell = world::compileSpell(*spellForm, &error);
        if (!spell) {
            LOG_WARN("Spell: {}", error);
            ctx.say(ctx.texts.get("spirit.refused"), 1.5f);
            return;
        }
        if (!world::spellSupported(*spell)) {
            LOG_WARN("Spell '{}': (form, element, trajectory) not implemented "
                     "by the generic caster yet — give the ability a script",
                     spellForm->editorId);
            ctx.say(ctx.texts.get("spirit.refused"), 1.5f);
            return;
        }
    }
    // Water pours into the LIVE water sim (runtime sources): without a
    // valid sim here the spirit stays silent. The other elements need
    // only the ground, in every world (story or sandbox).
    const bool waterCast =
        spell ? spell->element == render::terrain::SpiritKind::Water
              : spiritAbility->script.find("\"Water\"") != str::npos;
    if (waterCast && !ctx.renderer.waterSystem().simIsValid()) {
        ctx.say(ctx.texts.get("spirit.refused"), 1.5f);
        return;
    }
    // Geometry first, before any cost is paid: tryActivate has no
    // position. Only AIMED casts (a point spell, a script calling aim()):
    // water onto water would pour into a pinned lake (the pin swallows
    // it) — refused rather than silently lost. A stream arcs from the
    // hand and lands wherever it lands; nothing to pre-check.
    const bool aimed =
        spell ? spell->trajectory == world::SpellTrajectory::Point ||
                    spell->trajectory == world::SpellTrajectory::Line
              : spiritAbility->script.find("aim(") != str::npos;
    std::optional<Vec3> at;
    if (aimed) {
        at = ctx.aimGround();
        if (!at) {
            ctx.say(ctx.texts.get("spirit.noGround"), 1.5f);
            return;
        }
        if (spell &&
            glm::distance(*at, ctx.cameraPosition) > spell->range) {
            ctx.say(ctx.texts.get("spirit.tooFar"), 1.5f);
            return;
        }
        // Only a CREATE pours: destroy/control want the water there.
        const bool creates = (!spell || spell->verb == world::SpellVerb::Create) &&
                             (!spell || spell->element != render::terrain::SpiritKind::Wind);
        if (creates && render::terrain::waterSurfaceQuery(ctx.waterQuery(),
                                                          at->x, at->z, at->y)) {
            ctx.say(ctx.texts.get("spirit.wetGround"), 1.5f);
            return;
        }
    }
    if (spell && spell->verb == world::SpellVerb::Control && spiritHold) {
        return; // one held volume at a time
    }
    gameplay::AbilityContext ability { ctx.forms, ctx.tags };
    ability.events = &ctx.events;
    ability.caster = ctx.playerEntity;
    ability.scriptRunner = ctx.runAbilityScript;
    auto& set = ctx.playerEntity.get_mut<gameplay::AttributeSet>();
    auto& system = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
    if (!gameplay::tryActivate(*spiritAbility, set, system, set, system,
                               ability)) {
        ctx.say(ctx.texts.get("spirit.refused"), 1.5f);
        return;
    }
    if (spell) {
        executeSpell(ctx, *spell, at);
    }
}

void SpiritDirector::executeSpell(const SpiritContext& ctx,
                                  const world::SpellSpec& spell,
                                  const std::optional<Vec3>& aimedAt) {
    // The matrix (world/spirit/Spells kSpellMatrix) names the handler;
    // spellSupported gated the cast, so a missing cell is a no-op.
    const world::SpellCell* cell = world::spellCell(spell);
    if (!cell) {
        return;
    }
    // Point and line spells act at the pre-checked ground spot.
    const bool aimed = cell->trajectory == world::SpellTrajectory::Point ||
                       cell->trajectory == world::SpellTrajectory::Line;
    if (aimed && !aimedAt) {
        return;
    }
    // The area shape picks the water kernel footprint: a disc is the
    // kernel's own ~4 m disc, a ring the wide seven-tap spring
    // (world/spirit/SpiritSources).
    const f32 radius = spell.area == world::SpellArea::Ring
                           ? glm::max(spell.areaRadius,
                                      world::kSpellDiscMaxRadius + 0.01f)
                           : glm::min(spell.areaRadius,
                                      world::kSpellDiscMaxRadius);
    const f32 seconds = spell.duration < 0.0f ? 1.0e9f : spell.duration;
    PendingAction action;
    action.kind = spell.element;
    // Destroy = the same source, draining (the kernel bounds at dry and
    // releases the lake pin under it).
    action.rate = spell.verb == world::SpellVerb::Destroy ? -spell.intensity
                                                          : spell.intensity;
    action.radius = radius;
    action.seconds = spell.duration < 0.0f && !spell.channeled
                         ? -1.0f // a permanent placed source
                         : seconds;
    action.channeled = spell.channeled;
    action.costPeriod = spell.costPeriod;
    action.upkeepScale = spell.upkeepScale;
    if (cell->trajectory == world::SpellTrajectory::Point) {
        action.x = aimedAt->x;
        action.z = aimedAt->z;
    }
    switch (cell->effect) {
    case world::SpellEffect::WaterReading:
        castWaterReading(ctx, *aimedAt, spell.duration, spell.channeled);
        return;
    case world::SpellEffect::EarthReading:
        castGroundReading(ctx, *aimedAt, spell.duration, spell.channeled);
        return;
    case world::SpellEffect::FireReading:
        castFireReading(ctx, *aimedAt, spell.duration, spell.channeled);
        return;
    case world::SpellEffect::EarthWall:
        // The press spot; the release builds the wall (updateLine).
        spiritLine = SpiritLine { *aimedAt, spell.intensity, spell.areaRadius };
        return;
    case world::SpellEffect::WaterSource:
        action.mode = PendingAction::Mode::Source;
        break;
    case world::SpellEffect::WaterHold:
        action.mode = PendingAction::Mode::Hold;
        break;
    case world::SpellEffect::WaterJet:
        action.mode = PendingAction::Mode::Jet;
        action.speed = world::launchSpeedForRange(
            spell.range, world::SpiritJetList::kGravity);
        break;
    case world::SpellEffect::EarthBump:
        action.rate = spell.intensity; // the sign lives in the mode
        action.mode = PendingAction::Mode::EarthBump;
        break;
    case world::SpellEffect::EarthBrush:
        action.rate = spell.intensity;
        action.mode = PendingAction::Mode::EarthBrush;
        break;
    case world::SpellEffect::EarthDig:
        action.rate = spell.intensity;
        action.mode = PendingAction::Mode::EarthDig;
        break;
    case world::SpellEffect::EarthSeize:
        action.rate = spell.intensity;     // the largest rock it lifts
        action.radius = spell.areaRadius; // the reach around the aim
        action.mode = PendingAction::Mode::EarthSeize;
        break;
    case world::SpellEffect::FireIgnite:
        action.rate = spell.intensity; // heat dealt to each cell
        action.radius = spell.areaRadius;
        action.mode = PendingAction::Mode::FireIgnite;
        break;
    case world::SpellEffect::FireDouse:
        action.rate = spell.intensity;
        action.radius = spell.areaRadius;
        action.mode = PendingAction::Mode::FireDouse;
        break;
    case world::SpellEffect::FireBrand:
        action.rate = spell.intensity;
        action.radius = spell.areaRadius;
        action.mode = PendingAction::Mode::FireBrand;
        break;
    case world::SpellEffect::FireStream:
        action.rate = spell.intensity;
        action.radius = spell.areaRadius;
        action.range = spell.range;
        action.mode = PendingAction::Mode::FireStream;
        break;
    case world::SpellEffect::FireGlobe:
        action.radius = spell.areaRadius;
        action.mode = PendingAction::Mode::FireGlobe;
        break;
    case world::SpellEffect::WindGust: {
        // A gust placed at the aim, blowing where the caster faces.
        const Vec3 forward = ctx.cameraForward;
        const Vec2 flat = glm::length(Vec2 { forward.x, forward.z }) > 1e-3f
                              ? glm::normalize(Vec2 { forward.x, forward.z })
                              : Vec2 { 1.0f, 0.0f };
        action.dirX = flat.x;
        action.dirZ = flat.y;
        action.rate = spell.intensity; // m/s at the centre
        action.radius = spell.areaRadius;
        action.mode = PendingAction::Mode::Source;
        break;
    }
    case world::SpellEffect::WindBlow:
        action.rate = spell.intensity;
        action.radius = spell.areaRadius;
        action.range = spell.range;
        action.mode = PendingAction::Mode::WindBlow;
        break;
    case world::SpellEffect::WindCalm:
        action.mode = PendingAction::Mode::WindCalm;
        break;
    case world::SpellEffect::WindDirect:
        action.mode = PendingAction::Mode::WindDirect;
        break;
    }
    pendingActions.push_back(action);
}

void SpiritDirector::applyPendingActions(const SpiritContext& ctx) {
    if (pendingActions.empty()) {
        return;
    }
    bool changed = false;
    while (!pendingActions.empty()) {
        const PendingAction action = pendingActions.front();
        pendingActions.pop_front();
        if (action.kind == render::terrain::SpiritKind::Water &&
            !ctx.renderer.waterSystem().simIsValid()) {
            continue; // no live sim to pour into (a script's cast)
        }
        if (action.mode == PendingAction::Mode::FireDouse) {
            douse(action.x, action.z, action.radius);
            ctx.ambientFx.noteWaterEvent({ action.x, action.z }, action.radius);
            const Vec3 at { action.x,
                            render::terrain::height(ctx.terrainParams,
                                                    action.x, action.z),
                            action.z };
            ctx.cues.emit({ "Cue.Spirit.Fire.Douse", at, action.radius });
            continue;
        }
        if (action.mode == PendingAction::Mode::FireBrand) {
            endBrand(ctx);
            SpiritBrand brand;
            brand.heat = action.rate;
            brand.radius = action.radius;
            brand.costPeriod = action.costPeriod;
            brand.upkeepScale = action.upkeepScale;
            brand.ability = spiritAbility ? spiritAbility->id : core::Guid {};
            const core::Guid& fxId =
                fieldParticles(render::terrain::SpiritKind::Fire);
            if (const auto* form = ctx.forms.find<data::ParticleForm>(fxId)) {
                fx::EmitterParams params = gameplay::toEmitterParams(*form);
                params.duration = 1.0e9f; // until the key lets go
                params.shapeRadius = glm::min(params.shapeRadius, action.radius);
                const Vec3 at { action.x,
                                render::terrain::height(ctx.terrainParams,
                                                        action.x, action.z),
                                action.z };
                brand.emitter = ctx.fxSim.spawn(params, at, 0x6b7a9du ^ ctx.fxSim.count());
            }
            spiritBrand = brand;
            continue;
        }
        if (action.mode == PendingAction::Mode::WindBlow) {
            endBlow(ctx);
            SpiritBlow blow;
            blow.speed = action.rate;
            blow.radius = action.radius;
            blow.range = action.range;
            blow.costPeriod = action.costPeriod;
            blow.upkeepScale = action.upkeepScale;
            blow.ability = spiritAbility ? spiritAbility->id : core::Guid {};
            const core::Guid& fxId =
                jetParticles(render::terrain::SpiritKind::Wind);
            if (const auto* form = ctx.forms.find<data::ParticleForm>(fxId)) {
                fx::EmitterParams params = gameplay::toEmitterParams(*form);
                params.duration = 1.0e9f;
                params.velocity = ctx.cameraForward * action.rate;
                params.lifetime = action.range / glm::max(action.rate, 1.0f);
                blow.emitter = ctx.fxSim.spawn(params, nozzle(ctx), 0x91d0u ^ ctx.fxSim.count());
            }
            spiritBlow = blow;
            ctx.cues.emit({ "Cue.Spirit.Wind.Spawn", nozzle(ctx), blow.speed });
            continue;
        }
        if (action.mode == PendingAction::Mode::WindCalm ||
            action.mode == PendingAction::Mode::WindDirect) {
            if (!spiritWindHold) {
                spiritWindHold = SpiritWindHold {};
                windBaseStrength = ctx.atmos.windStrength;
                windBaseDirectionDeg = ctx.atmos.windDirectionDeg;
                spiritWindHold->currentDeg = ctx.atmos.windDirectionDeg;
            }
            spiritWindHold->releasing = false;
            spiritWindHold->calm = action.mode == PendingAction::Mode::WindCalm;
            spiritWindHold->direct = action.mode == PendingAction::Mode::WindDirect;
            spiritWindHold->costPeriod = action.costPeriod;
            spiritWindHold->upkeepScale = action.upkeepScale;
            spiritWindHold->costClock = 0.0f;
            spiritWindHold->ability = spiritAbility ? spiritAbility->id : core::Guid {};
            continue;
        }
        if (action.mode == PendingAction::Mode::FireStream) {
            endFlameJet(ctx);
            SpiritFlameJet jet;
            jet.heat = action.rate;
            jet.radius = action.radius;
            jet.range = action.range;
            jet.costPeriod = action.costPeriod;
            jet.upkeepScale = action.upkeepScale;
            jet.ability = spiritAbility ? spiritAbility->id : core::Guid {};
            const core::Guid& fxId =
                jetParticles(render::terrain::SpiritKind::Fire);
            if (const auto* form = ctx.forms.find<data::ParticleForm>(fxId)) {
                fx::EmitterParams params = gameplay::toEmitterParams(*form);
                params.duration = 1.0e9f; // until the key lets go
                params.velocity = ctx.cameraForward * kFlameJetSpeed;
                params.lifetime = action.range / kFlameJetSpeed;
                jet.emitter = ctx.fxSim.spawn(params, nozzle(ctx), 0x5e7f1a3u ^ ctx.fxSim.count());
            }
            spiritFlameJet = jet;
            continue;
        }
        if (action.mode == PendingAction::Mode::FireGlobe) {
            endGlobe(ctx);
            SpiritGlobe globe;
            globe.radius = action.radius;
            globe.costPeriod = action.costPeriod;
            globe.upkeepScale = action.upkeepScale;
            globe.ability = spiritAbility ? spiritAbility->id : core::Guid {};
            const auto* form = ctx.forms.find<data::ParticleForm>(
                holdParticles(render::terrain::SpiritKind::Fire));
            if (form && ctx.playerBody) {
                fx::EmitterParams params = gameplay::toEmitterParams(*form);
                params.duration = 1.0e9f;
                params.shapeRadius = action.radius * 0.5f;
                const Vec3 at = ctx.playerBody->position() + Vec3 { 0.0f, 1.0f, 0.0f };
                globe.emitter = ctx.fxSim.spawn(params, at, 0x4a11e5u ^ ctx.fxSim.count());
            }
            spiritGlobe = globe;
            continue;
        }
        if (action.mode == PendingAction::Mode::FireIgnite) {
            igniteGround(ctx, action.x, action.z, action.radius, action.rate);
            const Vec3 at { action.x,
                            render::terrain::height(ctx.terrainParams,
                                                    action.x, action.z),
                            action.z };
            ctx.cues.emit({ "Cue.Spirit.Fire.Spawn", at, action.rate });
            continue;
        }
        if (action.mode == PendingAction::Mode::EarthBump) {
            applyEarthBump(ctx, action);
            continue; // no water source involved
        }
        if (action.mode == PendingAction::Mode::EarthSeize) {
            seizeRock(ctx, action);
            continue;
        }
        if (action.mode == PendingAction::Mode::EarthDig ||
            action.mode == PendingAction::Mode::EarthBrush) {
            finishEarth(ctx, true); // one stroke at a time
            SpiritEarth dig;
            dig.raise = action.mode == PendingAction::Mode::EarthBrush;
            dig.rate = action.rate;
            dig.radius = action.radius;
            dig.remaining = action.seconds;
            dig.costPeriod = action.costPeriod;
            dig.upkeepScale = action.upkeepScale;
            dig.ability = spiritAbility ? spiritAbility->id : core::Guid {};
            spiritEarth = std::move(dig);
            continue;
        }
        if (action.mode == PendingAction::Mode::Hold) {
            world::SpiritHold hold;
            hold.kind = action.kind;
            hold.rate = action.rate;
            hold.radius = action.radius;
            hold.maxVolume = action.rate * action.seconds;
            hold.costPeriod = action.costPeriod;
            hold.upkeepScale = action.upkeepScale;
            hold.ability = spiritAbility ? spiritAbility->id : core::Guid {};
            hold.aim = Vec3 { action.x,
                              render::terrain::height(ctx.terrainParams,
                                                      action.x, action.z),
                              action.z };
            const core::Guid& fxId = holdParticles(action.kind);
            if (const auto* form = ctx.forms.find<data::ParticleForm>(fxId)) {
                fx::EmitterParams params = gameplay::toEmitterParams(*form);
                params.duration = 1.0e9f; // stopped by the release
                const Vec3 blob = *hold.aim + Vec3 { 0.0f, 2.5f, 0.0f };
                const u32 seed =
                    static_cast<u32>(static_cast<i32>(blob.x * 73.0f)) ^
                    (static_cast<u32>(static_cast<i32>(blob.z * 179.0f)) << 8) ^
                    ctx.fxSim.count();
                hold.emitter = ctx.fxSim.spawn(params, blob, seed);
            }
            releaseHold(ctx, false);
            spiritHold = hold;
            changed = true;
            continue;
        }
        if (action.mode == PendingAction::Mode::Jet) {
            world::SpiritJet jet;
            jet.kind = action.kind;
            jet.origin = nozzle(ctx);
            jet.velocity = ctx.cameraForward * action.speed;
            jet.rate = action.rate;
            jet.radius = action.radius;
            jet.remaining = action.seconds;
            jet.channeled = action.channeled;
            jet.costPeriod = action.costPeriod;
            jet.upkeepScale = action.upkeepScale;
            jet.ability = spiritAbility ? spiritAbility->id : core::Guid {};
            // The visible stream: the spirit's jet ParticleForm, its
            // duration bound to the gesture; the per-frame steer keeps
            // the arc true to the aim.
            const core::Guid& fxId = jetParticles(action.kind);
            if (const auto* form = ctx.forms.find<data::ParticleForm>(fxId)) {
                fx::EmitterParams params = gameplay::toEmitterParams(*form);
                params.velocity = jet.velocity;
                params.gravity = { 0.0f, -world::SpiritJetList::kGravity,
                                   0.0f };
                params.duration = action.seconds;
                const u32 seed =
                    static_cast<u32>(static_cast<i32>(jet.origin.x * 73.0f)) ^
                    (static_cast<u32>(static_cast<i32>(jet.origin.z * 179.0f))
                     << 8) ^
                    ctx.fxSim.count();
                jet.emitter = ctx.fxSim.spawn(params, jet.origin, seed);
            }
            if (const u32 evicted = jetList().start(jet)) {
                ctx.fxSim.stopEmitter(evicted);
            }
            changed = true;
            continue;
        }
        spawn(action.kind, action.x, action.z, action.rate, action.radius,
              action.seconds, ctx.worldspace, action.dirX, action.dirZ);
        if (action.kind == render::terrain::SpiritKind::Water) {
            ctx.ambientFx.noteWaterEvent({ action.x, action.z }, action.radius);
        }
        changed = true;
        const Vec3 at { action.x,
                        render::terrain::height(ctx.terrainParams,
                                                action.x, action.z),
                        action.z };
        ctx.cues.emit(
            { "Cue.Spirit." +
                  str { render::terrain::spiritName(action.kind) } +
                  ".Spawn",
              at, action.rate });
    }
    if (changed) {
        updateJets(ctx, 0.0f);
        ctx.pushWaterSources();
    }
}

void SpiritDirector::update(const SpiritContext& ctx, f32 dt) {
    updateJets(ctx, dt);
    updateHold(ctx, dt);
    updateEarth(ctx, dt);
    updateLine(ctx);
    updateRocks(ctx, dt);
    updateReading(ctx, dt);
    updateBrand(ctx, dt);
    updateGlobe(ctx, dt);
    updateFlameJet(ctx, dt);
    updateBlow(ctx, dt);
    updateWind(ctx, dt);
}

void SpiritDirector::resetGestures(const SpiritContext& ctx, ResetReason reason) {
    // The steps are independent of one another (each ends its own
    // gesture and hands back its own emitters), so one order serves both
    // callers; the two lists differ only in what the flags below pick.
    const bool clearSources = reason == ResetReason::ConsoleClear;
    const bool commitStroke = reason == ResetReason::ConsoleClear;
    const bool worldGone = reason == ResetReason::MapChanged;
    if (clearSources) {
        sources.clear();
    }
    stopEmitters(ctx.fxSim, jets.clear());
    releaseHold(ctx, false);
    finishEarth(ctx, commitStroke);
    endBrand(ctx);
    endGlobe(ctx);
    endFlameJet(ctx);
    resetWind(ctx);
    if (worldGone) {
        spiritLine.reset();
        releaseRocks(ctx);
    }
    ctx.pushWaterSources();
}

void SpiritDirector::updateReading(const SpiritContext& ctx, f32 dt) {
    if (spiritReadingLive) {
        // Held: re-read where the aim rests now (live current, volume);
        // released: gone.
        if (!ctx.castHeld ||
            !ctx.playerEntity.is_alive()) {
            spiritReadingLive = false;
            spiritReadingSeconds = 0.0f;
            spiritReading.clear();
            return;
        }
        if (const std::optional<Vec3> at = ctx.aimGround()) {
            switch (spiritReadingKind) {
            case ReadingKind::Ground:
                castGroundReading(ctx, *at, 0.0f, true);
                break;
            case ReadingKind::Fire:
                castFireReading(ctx, *at, 0.0f, true);
                break;
            case ReadingKind::Water:
                castWaterReading(ctx, *at, 0.0f, true);
                break;
            }
        }
        return;
    }
    spiritReadingSeconds = glm::max(0.0f, spiritReadingSeconds - dt);
    if (spiritReadingSeconds <= 0.0f) {
        spiritReading.clear();
    }
}

void SpiritDirector::extract(render::RenderSnapshot& out, f32 timeSeconds) const {
    // Jets: the lumps in flight, one low-poly sphere each (a jet's
    // spheres share one mesh instance).
    u64 id = 1;
    for (const world::SpiritJet& jet : jetList().entries()) {
        if (jet.kind != render::terrain::SpiritKind::Water ||
            jet.spheres.empty()) {
            continue;
        }
        render::RenderSnapshot::WaterMeshInstance mesh;
        mesh.id = 0x5b1e0000u + id++;
        for (const world::JetSphere& sphere : jet.spheres) {
            world::appendBlob(mesh.triangles,
                              sphere.at(world::SpiritJetList::kGravity),
                              sphere.radius, 5, 8, 0.9f);
        }
        out.waterMeshes.push_back(std::move(mesh));
    }
    // The carried volume: a blob whose size IS the volume, floating over
    // the aim with a slow bob.
    if (spiritHold && spiritHold->aim &&
        spiritHold->kind == render::terrain::SpiritKind::Water) {
        render::RenderSnapshot::WaterMeshInstance mesh;
        mesh.id = 0x5b1e0000u + 0x100u;
        const f32 radius = world::blobRadius(spiritHold->volume);
        const Vec3 center = *spiritHold->aim +
                            Vec3 { 0.0f, 2.5f + radius * 0.5f +
                                             0.15f * std::sin(timeSeconds * 2.0f),
                                   0.0f };
        world::appendBlob(mesh.triangles, center, radius);
        out.waterMeshes.push_back(std::move(mesh));
    }
}

void SpiritDirector::buildSpellBook(const data::FormDatabase& forms) {
    spellBook.clear();
    data::forEach<data::SpellForm>(forms, [&](const data::SpellForm& spell) {
        const auto* ability =
            forms.find<gameplay::AbilityForm>(spell.parent);
        if (ability && std::find(spellBook.begin(), spellBook.end(),
                                 ability) == spellBook.end()) {
            spellBook.push_back(ability);
        }
    });
    std::sort(spellBook.begin(), spellBook.end(),
              [](const gameplay::AbilityForm* a,
                 const gameplay::AbilityForm* b) {
                  return a->editorId < b->editorId;
              });
    spiritAbility = nullptr;
    for (const gameplay::AbilityForm* ability : spellBook) {
        if (ability->editorId == "SpiritWaterJet") {
            spiritAbility = ability;
        }
    }
    if (!spiritAbility && !spellBook.empty()) {
        spiritAbility = spellBook.front();
    }
}

void SpiritDirector::cycleSpell(i32 direction) {
    if (spellBook.empty()) {
        return;
    }
    const auto it = std::find(spellBook.begin(), spellBook.end(), spiritAbility);
    const i32 count = static_cast<i32>(spellBook.size());
    const i32 current =
        it == spellBook.end() ? 0 : static_cast<i32>(it - spellBook.begin());
    spiritAbility = spellBook[static_cast<size_t>(
        ((current + direction) % count + count) % count)];
}

str SpiritDirector::currentSpellName(const data::FormDatabase& forms,
                                     const data::TextTable& texts) const {
    if (!spiritAbility) {
        return {};
    }
    str name;
    data::childrenOf<data::SpellForm>(
        forms, spiritAbility->id, [&](const data::SpellForm& spell) {
            if (name.empty()) {
                name = spell.name.empty() ? spell.editorId
                                          : texts.get(spell.name);
            }
        });
    return name.empty() ? spiritAbility->editorId : name;
}

} // namespace game
