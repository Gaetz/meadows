#pragma once

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_set>

#include "data/forms/FormDatabase.hpp"
#include "engine/core/ConcurrentQueue.hpp"
#include "engine/ecs/World.hpp"       // ecs::Entity, flecs::query
#include "engine/physics/Physics.hpp" // phys::BodyId
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/terrain/HeightPatches.hpp"
#include "engine/terrain/SpiritField.hpp"
#include "engine/terrain/WaterBodies.hpp"
#include "engine/terrain/WaterQuery.hpp"
#include "engine/terrain/WaterSim.hpp"
#include "engine/terrain/WindField.hpp"
#include "engine/terrain/generation/WaterSolve.hpp"
#include "game/scenes/StreamingController.hpp" // WorldPolicy
#include "world/scene/Components.hpp"          // world::Transform, RefId
#include "world/spirit/SpiritFire.hpp"
#include "world/spirit/SpiritJets.hpp"
#include "world/spirit/SpiritRules.hpp"
#include "world/spirit/SpiritSources.hpp"
#include "world/spirit/Spells.hpp"
#include "world/spirit/WaterReading.hpp"
#include "world/terrain/TerrainBrush.hpp"

namespace core {
class JobSystem;
}
namespace data {
class TextTable;
}
namespace fx {
class ParticleSim;
}
namespace gameplay {
struct AbilityForm;
class CueRegistry;
class EventBus;
class GameplayTagRegistry;
}
namespace phys {
class CharacterBody;
class PhysicsWorld;
}
namespace render {
struct AtmosphereParams;
class MeshCache;
struct RenderSnapshot;
class WorldRenderer;
}

namespace game {

class AmbientFxController;
struct Npc;

// The scene systems the spirit gestures read and act on, bundled so the
// cast, the held gestures and their resets are decoupled from
// LandscapeScene. The scene rebuilds it each call from its own members —
// references, a few scalars, and the actions that stay scene territory as
// closures (the InteractionContext pattern). Mirrors FireContext.
//
// The committed terrain overlay arrives as a reference to the scene's
// handle, not a copy: a committed republish swaps it mid-call and the
// next gesture must read the new one.
struct SpiritContext {
    const data::FormDatabase& forms;      // AbilityForms, SpellForms, ParticleForms, StaticForms
    const data::TextTable& texts;         // the toasts and the readings' lines
    const gameplay::GameplayTagRegistry& tags;
    gameplay::EventBus& events;           // the ability activation publishes into it
    fx::ParticleSim& fxSim;
    gameplay::CueRegistry& cues;
    AmbientFxController& ambientFx;       // the ground events (a splash snuffs a torch)
    render::WorldRenderer& renderer;      // the live water sim
    render::TerrainParams& terrainParams; // the scene's world params (the earth gestures swap the overlay)
    phys::PhysicsWorld* physics;          // null before the world exists
    render::MeshCache* meshes;            // the rocks' hulls; null before the GPU resources exist
    flecs::query<const world::Transform, const world::RefId>& interactQuery; // the rock props
    const vector<uptr<Npc>>& npcs;        // who stands in a jet, who the ground throws
    ecs::Entity playerEntity;
    phys::CharacterBody* playerBody;      // null outside Play
    Vec3 cameraPosition;
    Vec3 cameraForward;
    bool playMode;                        // mode == Play
    bool castHeld;                        // InputAction::SpiritCast is down this frame
    WorldPolicy policy;                   // allowsSpirits gates the jets' bite
    const sptr<const render::HeightPatches>& heightPatches; // the committed overlay
    render::AtmosphereParams& atmos;      // the weather's wind, bent by the wind holds
    bool weatherTransitioning;            // a crossfade rewrites the atmosphere each frame
    core::Guid worldspace;                // the active worldspace (sources are tagged with it)
    // The frame's gameplay water query (build at call time, same frame).
    std::function<render::terrain::WaterQuery()> waterQuery;
    // The aimed ground point (the eye ray against physics); nullopt = nothing.
    std::function<std::optional<Vec3>()> aimGround;
    std::function<void(const str& line, f32 seconds)> say;
    // The terrain republish funnel (the sculpt tool's): preview or commit.
    std::function<void(std::shared_ptr<render::HeightPatches> next,
                       const std::vector<u64>& changed, bool commit)>
        republishTerrain;
    // The kernel's runtime sources re-pushed (springs + jets + the hold's draw).
    std::function<void()> pushWaterSources;
    // The ability's Lua script started as a coroutine over the player.
    std::function<void(const gameplay::AbilityForm&)> runAbilityScript;
    // A prop handed to the spirit: its static collider leaves the world.
    std::function<void(ecs::Entity prop)> retireCollider;
};

// The scene-side owner of the spirit framework (docs/CHANTIER-ESPRITS.md):
// the placed sources, the transient jets, the compiled rule table, the
// fire lane — the one spirit job in flight, distinct from the water's —
// and the caster's gestures (docs/SPELLS.md): the cast, the queued
// actions applied at the frame's safe point, the held gestures of every
// element, the readings, the spell book, the wind field the world reads.
// Main thread only; the kernels get VALUE copies at their own enqueue.
class SpiritDirector {
public:
    void build(const data::FormDatabase& forms); // rules + authored/saved sources

    core::Guid spawn(render::terrain::SpiritKind kind, f32 x, f32 z,
                     f32 rate, f32 radius, f32 durationSimSeconds,
                     const core::Guid& worldspace, f32 dirX = 0.0f,
                     f32 dirZ = 0.0f);
    bool remove(const core::Guid& id) { return sources.remove(id); }
    void clear() { sources.clear(); }
    // Lifetimes run in SIM seconds. True when the set changed (sources
    // or jets); `stoppedEmitters` receives the expired jets' handles.
    bool tick(f32 simSeconds, vector<u32>* stoppedEmitters = nullptr) {
        const bool a = sources.tick(simSeconds);
        const bool b = jets.tick(simSeconds, stoppedEmitters);
        return a || b;
    }
    // The particle emitters a tick (or a jet eviction) handed back.
    static void stopEmitters(fx::ParticleSim& fxSim, const vector<u32>& emitters);

    // Jets: the caster's streamed gesture (world/spirit/SpiritJets).
    world::SpiritJetList& jetList() { return jets; }
    const world::SpiritJetList& jetList() const { return jets; }
    // The ParticleForm a jet of `kind` streams (null guid = none).
    const core::Guid& jetParticles(render::terrain::SpiritKind kind) const {
        return jetFx[static_cast<size_t>(kind)];
    }
    const core::Guid& holdParticles(render::terrain::SpiritKind kind) const {
        return holdFx[static_cast<size_t>(kind)];
    }
    // The ParticleForm of one ACTIVE cell of the field (flames).
    const core::Guid& fieldParticles(render::terrain::SpiritKind kind) const {
        return fieldFx[static_cast<size_t>(kind)];
    }
    // The ParticleForm of the sparks an active cell sheds.
    const core::Guid& sparkParticles(render::terrain::SpiritKind kind) const {
        return sparkFx[static_cast<size_t>(kind)];
    }
    // The ParticleForm of the smoke an active cell sheds.
    const core::Guid& smokeParticles(render::terrain::SpiritKind kind) const {
        return smokeFx[static_cast<size_t>(kind)];
    }
    const core::Guid& hazeParticles(render::terrain::SpiritKind kind) const {
        return hazeFx[static_cast<size_t>(kind)];
    }
    // The SoundForm looped near the field's active cells.
    const core::Guid& fieldSound(render::terrain::SpiritKind kind) const {
        return soundFx[static_cast<size_t>(kind)];
    }
    // The earth spirit's lift (SpiritForm Earth): the speed a rising
    // ground throws a character with, from how deep it sank into it.
    struct EarthLift {
        f32 quadratic { 2.0f };
        f32 min { 1.5f };
        f32 max { 25.0f };
        f32 speedFor(f32 depth) const {
            return glm::clamp(quadratic * depth * depth, min, max);
        }
    };
    const EarthLift& earthLift() const { return lift; }
    // The control spell's draw: one extra (draining) kernel source while
    // a hold draws water, none otherwise.
    void setHoldSource(std::optional<render::terraingen::WaterSource> src) {
        holdSource = std::move(src);
    }

    // The kernel view of one map: placed springs + every landed jet.
    vector<render::terraingen::WaterSource> waterSources(
        const core::Guid& worldspace) const {
        vector<render::terraingen::WaterSource> out =
            sources.waterSourcesFor(world::WorldspaceFilter { worldspace });
        jets.appendWaterSources(out);
        if (holdSource) {
            out.push_back(*holdSource);
        }
        return out;
    }
    // The save's records: the sources, and the fire's cells when it burns
    // (a FireStateForm — worldspace-tagged, restored by restoreFire).
    vector<data::Record> capture(const core::Guid& worldspace) const;
    // A loaded save's fire: its cells go into the next job's window (the
    // lane wakes for it).
    void restoreFire(const world::FireStateForm& form);
    const world::SpiritSourceList& list() const { return sources; }
    const world::SpiritRuleTable& rules() const { return table; }
    // The fuel blend resolved once from the table (world/spirit/SpiritFire).
    const world::GroundProps& groundFuel() const { return groundProps; }

    // --- The gestures (docs/SPELLS.md) ----------------------------------
    // The ability -> world seam. A script's `spirit.spawn(...)` runs
    // mid-frame (a coroutine resume, a trigger dispatch) so it only
    // QUEUES here; the same safe point as the queued map crossing
    // applies. executeSpell fills one from the spell's matrix cell
    // (world::kSpellMatrix); Source also serves the script path.
    struct PendingAction {
        render::terrain::SpiritKind kind {};
        f32 x { 0.0f };
        f32 z { 0.0f };
        f32 rate { 0.0f };
        f32 radius { 0.0f };
        f32 seconds { 0.0f };
        // Source: placed at (x, z). Jet: streamed from the caster's
        // nozzle along the aim at `speed`. Hold: the control spell's
        // held volume, drawn at the aim while the key is held.
        // EarthBump: an instant mound at (x, z) that throws what stands
        // on it. EarthDig: the ground lowered under the aim while held.
        // EarthBrush: the ground raised (create) or lowered (destroy) under
        // the aim while held. EarthWall: a ridge from the press spot to the
        // release spot.
        // EarthSeize: the nearest rock within `radius` of the aim, no
        // bigger than `rate` metres, carried while held.
        // FireIgnite: `rate` heat dealt to the fire field's cells within
        // `radius` of (x, z) — the field spreads it.
        // FireDouse: the burning cells within `radius` of (x, z) put out.
        // FireBrand: a flame carried at the aim while held, igniting
        // `radius` around it with `rate` heat every costPeriod.
        // FireGlobe: the fire ward around the caster while held — nothing
        // burns within `radius`, the caster feels no flame.
        // FireStream: the flame jet — a short cone ahead of the caster while
        // held, igniting the ground along it (`rate` heat, `radius`) and
        // burning whoever stands in it, out to `range`.
        // WindBlow: the breath — a gust carried at the aim while held.
        // WindCalm / WindDirect: the weather's wind stilled / steered
        // where the caster faces, while held.
        enum class Mode : u8 { Source, Jet, Hold, EarthBump, EarthBrush,
                               EarthDig, EarthWall, EarthSeize, FireIgnite,
                               FireDouse, FireBrand, FireGlobe, FireStream,
                               WindBlow, WindCalm, WindDirect };
        Mode mode { Mode::Source };
        f32 speed { 0.0f };
        f32 range { 0.0f };
        f32 dirX { 0.0f }; // wind sources: the direction they blow toward
        f32 dirZ { 0.0f };
        bool channeled { false };
        f32 costPeriod { 1.0f };
        f32 upkeepScale { 0.25f };
    };
    // The script path (Vm::bindWorldActions): a placed source, a jet.
    void queueSource(render::terrain::SpiritKind kind, f32 x, f32 z, f32 rate,
                     f32 radius, f32 seconds) {
        pendingActions.push_back(PendingAction { kind, x, z, rate, radius, seconds });
    }
    void queueJet(render::terrain::SpiritKind kind, f32 speed, f32 rate,
                  f32 radius, f32 seconds) {
        PendingAction action { kind, 0.0f, 0.0f, rate, radius, seconds };
        action.mode = PendingAction::Mode::Jet;
        action.speed = speed;
        pendingActions.push_back(action);
    }
    // The frame's safe point (after every ECS iteration of the sim
    // section): the queued actions become sources, jets and gestures.
    void applyPendingActions(const SpiritContext& ctx);

    // Q in Play: the geometric pre-checks (aim, dry ground) then the
    // ability activation — cost/cooldown are its effects (§6), its
    // script or its compiled SpellForm acts on the world.
    void cast(const SpiritContext& ctx);
    // Right after the physics tick: the seized rocks' entities follow
    // their bodies (rolling, resting); a rock whose cell unloaded goes.
    void syncRocks(phys::PhysicsWorld& physics, const render::TerrainParams& terrain);
    // Right after the weather crossfade, every frame (paused or not): the
    // calmed / steered wind eases toward its target or back to the weather's.
    void updateWindHold(const SpiritContext& ctx, f32 dt);
    // The Play block, once per frame with the player on foot: the jets,
    // the hold, the earth stroke, the wall, the rocks, the reading, the
    // brand, the globe, the flame jet, the breath, then the wind field —
    // in this order.
    void update(const SpiritContext& ctx, f32 dt);
    // The gestures' water as geometry (jet tubes, the carried blob) into
    // the snapshot — drawn with the placed-volume water look.
    void extract(render::RenderSnapshot& out, f32 timeSeconds) const;
    // Every caller of the gestures' reset ends every held gesture and
    // the jets, then re-pushes the kernel's sources; they differ on the
    // rest. MapChanged (the world swapped under the caster): the stroke
    // in progress is abandoned, the wall's press spot and the seized
    // rocks go with the world, the sources stay (the map's own). The
    // console's clear: the sources go too, the stroke commits, the wall
    // and the rocks are left as they are. SceneExit (the scene leaves,
    // a reload being onExit + onEnter): everything goes — sources, the
    // stroke abandoned, rocks released — so nothing of this world leaks
    // into the next one; build() re-reads the sources from the records.
    // The fire director and the ambient fx reset beside it
    // (LandscapeScene::resetWorldFx).
    enum class ResetReason : u8 { MapChanged, ConsoleClear, SceneExit };
    void resetGestures(const SpiritContext& ctx, ResetReason reason);

    // The spell book (draft UI): every ability carrying a SpellForm, in
    // editorId order; the wheel cycles it, the HUD names the current one.
    // The console's `spirit cast` selects by hand.
    void buildSpellBook(const data::FormDatabase& forms);
    void cycleSpell(i32 direction);
    void selectSpell(const gameplay::AbilityForm* ability) { spiritAbility = ability; }
    str currentSpellName(const data::FormDatabase& forms,
                         const data::TextTable& texts) const;
    // The reading block the HUD shows while one is open (empty = hidden).
    vector<str> readingLines() const {
        return spiritReadingSeconds > 0.0f || spiritReadingLive ? spiritReading
                                                                : vector<str> {};
    }

    // What the fire's presentation and the actors share with the gestures.
    // Actors standing in the flame jet THIS frame: the contact pass reads
    // it beside the ground's own fire (it would otherwise reset their
    // contact clock every job, the ground under them being unlit).
    const std::unordered_set<u64>& jetTouched() const { return flameJetTouched; }
    // The fire ward is up: no flame reaches the caster.
    bool warded() const { return spiritGlobe.has_value(); }
    // The world's wind FIELD: the weather's global wind (direction and
    // strength x kWindSpeedPerStrength m/s) plus the placed gusts (Wind
    // SpiritSources) and the breath's carried gust; the fire job, the
    // particles, the floaters and the actors read it.
    const render::terrain::WindField& wind() const { return worldWind; }
    static constexpr f32 kWindSpeedPerStrength = 5.0f; // m/s at weather strength 1
    static constexpr f32 kPlayerWindPush = 0.6f;       // of the gusts' speed
    static constexpr f32 kFloaterWindDrift = 0.08f;    // of the wind, on a floating prop

    // --- The fire lane (world/spirit/SpiritFire) ------------------------
    // A camera window of the fire field stepped at 10 Hz on a worker, one
    // job in flight; the window follows the focus (scroll), the mask and
    // the burning centers are what the last landed job published. Idle
    // (no job at all) until a spark, and again once nothing burns.
    struct FireFrame {
        const render::TerrainParams* params { nullptr };
        // The live sim snapshot when the display shows it, else null:
        // the baked bodies answer (the WaterQuery contract).
        sptr<const render::terrain::WaterSimSnapshot> water;
        sptr<const render::WaterBodies> bodies;
        f32 seaLevel { -1.0e6f };
        Vec2 focus { 0.0f }; // camera XZ
        f32 rain { 0.0f };   // the weather's rain 0..1 (FireParams::rain)
    };
    // Heat dealt to every cell within `radius` of (x, z) by the next job.
    void ignite(f32 x, f32 z, f32 radius, f32 heat);
    // Every burning cell within `radius` of (x, z) put out by the next job.
    void douse(f32 x, f32 z, f32 radius);
    // A douse repeated by EVERY job while set (the fire ward around a
    // character): nothing burns inside it.
    void setWard(std::optional<world::FireDouse> ward) { fireWard = std::move(ward); }
    // Once per frame after the safe point. True when a job landed this
    // frame (fireScorch / fireBurning are fresh).
    bool updateFire(core::JobSystem& jobs, const FireFrame& frame,
                    f32 simSeconds);
    void resetFire(); // map swap / exit: the window is dropped
    // Runs a job even while nothing burns (a torch heating a prop: the
    // job is what ticks the props and trees). Consumed by the next job.
    void wakeFire() { fireWake = true; }
    // The wind field the fire spreads with (m/s; the kernel scales it
    // to 0..1 by windFullSpeed), a value copy per job.
    void setWindField(const render::terrain::WindField& field) { laneWind = field; }
    static constexpr f32 kWindFullSpeed = 10.0f; // m/s = a full (1.0) fire wind
    bool fireIdle() const {
        return !fireGrid && !fireInFlight && fireIgnitions.empty();
    }
    const vector<u8>& fireScorch() const { return fireMask; }
    const vector<u8>& fireGlow() const { return fireGlowMask; }
    // The ember level at a point, 0..1 (0 outside the window / cold).
    f32 fireGlowAt(f32 x, f32 z) const;
    // The scorch at a point, 0..1 (0 outside the window / untouched).
    f32 fireScorchAt(f32 x, f32 z) const;
    // Is the cell at a point burning right now (the gameplay truth for
    // contact and props: a cell burns for its whole fuel, well past the
    // front's glow).
    bool fireBurningAt(f32 x, f32 z) const;
    const world::TreeFireParams& treeFire() const { return treeFireParams; }
    // Typed fire damage dealt with each contact (SpiritForm.contactDamage).
    f32 contactDamage(render::terrain::SpiritKind kind) const {
        return contactHurt[static_cast<size_t>(kind)];
    }
    // The EffectForm actors standing in the field take (SpiritForm
    // contactEffect), every contactPeriod seconds.
    const core::Guid& contactEffect(render::terrain::SpiritKind kind) const {
        return contactFx[static_cast<size_t>(kind)];
    }
    f32 contactPeriod(render::terrain::SpiritKind kind) const {
        return contactEvery[static_cast<size_t>(kind)];
    }
    const render::terraingen::GridSpec& fireSpec() const { return fireMaskSpec; }
    const vector<Vec2>& fireBurning() const { return fireCenters; }
    const render::terrain::FireStats& fireStats() const { return lastFireStats; }
    f32 fireLastMs() const { return fireMs; }

private:
    // --- The framework --------------------------------------------------
    world::SpiritSourceList sources;
    world::SpiritJetList jets;
    world::SpiritRuleTable table;
    std::array<core::Guid,
               static_cast<size_t>(render::terrain::SpiritKind::kCount)>
        jetFx {};
    std::array<core::Guid,
               static_cast<size_t>(render::terrain::SpiritKind::kCount)>
        holdFx {};
    std::array<core::Guid,
               static_cast<size_t>(render::terrain::SpiritKind::kCount)>
        fieldFx {};
    std::array<core::Guid,
               static_cast<size_t>(render::terrain::SpiritKind::kCount)>
        sparkFx {};
    std::array<core::Guid,
               static_cast<size_t>(render::terrain::SpiritKind::kCount)>
        soundFx {};
    std::array<core::Guid,
               static_cast<size_t>(render::terrain::SpiritKind::kCount)>
        smokeFx {};
    std::array<core::Guid,
               static_cast<size_t>(render::terrain::SpiritKind::kCount)>
        hazeFx {};
    std::optional<render::terraingen::WaterSource> holdSource;
    EarthLift lift;

    // --- The gestures ---------------------------------------------------
    // A compiled SpellForm (docs/SPELLS.md) into world actions; `aimedAt`
    // is the pre-checked ground spot for point spells.
    void executeSpell(const SpiritContext& ctx, const world::SpellSpec& spell,
                      const std::optional<Vec3>& aimedAt);
    std::deque<PendingAction> pendingActions;
    // The caster's nozzle (eye ray, hand-height offset).
    static Vec3 nozzle(const SpiritContext& ctx);
    // The spirit ability behind Q: its Lua script acts through the world
    // actions bound on the Vm, or its SpellForm compiles into actions.
    const gameplay::AbilityForm* spiritAbility { nullptr };
    vector<const gameplay::AbilityForm*> spellBook;

    // Water (SpiritWater.cpp). Jets follow the caster every frame: re-aim,
    // re-land, steer their particle stream, feed the water kernel.
    void updateJets(const SpiritContext& ctx, f32 dt);
    // The stream is lumps launched at this cadence (seconds).
    static constexpr f32 kJetSphereInterval = 0.12f;
    // A water jet's lump passing through an actor shoves him along its
    // flight: this many m/s of shove gained per second in the stream,
    // capped (the stream flows at ~24 m/s; a character is not a leaf).
    static constexpr f32 kWaterJetShoveRate = 24.0f;
    static constexpr f32 kWaterJetShoveMax = 6.0f;
    // The control spell's held volume (world/spirit/SpiritJets
    // SpiritHold): drawn at the aim while Q is held, dropped on release.
    std::optional<world::SpiritHold> spiritHold;
    void updateHold(const SpiritContext& ctx, f32 dt);
    void releaseHold(const SpiritContext& ctx, bool drop);
    // Understand x Water: the reading shown by the HUD for a while
    // (world/spirit/WaterReading, formatted here from the loc keys).
    void castWaterReading(const SpiritContext& ctx, const Vec3& at, f32 seconds,
                          bool live);

    // Earth (SpiritEarth.cpp). The dig in progress (world/terrain/
    // TerrainBrush working grids, previewed at ~20 Hz, committed once on
    // release), and the bump. rate = metres per second lowered (dig) /
    // metres raised (bump), divided by the ground's hardness
    // (SurfaceMaterialForm).
    struct SpiritEarth {
        world::BrushGrids grids;
        bool raise { false }; // the stone brush raises, the dig lowers
        f32 rate { 0.0f };
        f32 radius { 4.0f };
        f32 remaining { 0.0f };
        f32 previewTimer { 0.0f };
        f32 cueTimer { 0.0f };
        f32 costPeriod { 1.0f };
        f32 costClock { 0.0f };
        f32 upkeepScale { 0.25f };
        core::Guid ability;
    };
    std::optional<SpiritEarth> spiritEarth;
    void applyEarthBump(const SpiritContext& ctx, const PendingAction& action);
    void updateEarth(const SpiritContext& ctx, f32 dt);
    void finishEarth(const SpiritContext& ctx, bool commit);
    f32 groundHardnessAt(const SpiritContext& ctx, f32 x, f32 z) const;
    // Rocks the earth spirit seized: their static body became a dynamic
    // convex hull (engine/physics), carried kinematically while held,
    // free afterwards — the entity's Transform follows the body.
    struct SpiritRock {
        ecs::Entity entity;
        phys::BodyId body { 0 };
        bool held { false };
        f32 radius { 1.0f };
        Vec3 lastTarget { 0.0f };
        f32 costPeriod { 1.0f };
        f32 costClock { 0.0f };
        f32 upkeepScale { 0.25f };
        core::Guid ability;
    };
    vector<SpiritRock> spiritRocks;
    void seizeRock(const SpiritContext& ctx, const PendingAction& action);
    void updateRocks(const SpiritContext& ctx, f32 dt);
    void releaseRocks(const SpiritContext& ctx); // map swap: bodies go with the world
    // The wall gesture: the press spot, the release spot builds the ridge.
    struct SpiritLine {
        Vec3 start { 0.0f };
        f32 height { 0.0f };
        f32 halfWidth { 1.5f };
    };
    std::optional<SpiritLine> spiritLine;
    void updateLine(const SpiritContext& ctx);
    // Understand x Earth: the ground under the aim, same HUD block.
    void castGroundReading(const SpiritContext& ctx, const Vec3& at, f32 seconds,
                           bool live);

    // Fire (SpiritFire.cpp). A spell's spark on the ground: the lane's
    // ignition, remembered for the frame (the props that relight from it).
    void igniteGround(const SpiritContext& ctx, f32 x, f32 z, f32 radius, f32 heat);
    std::unordered_set<u64> flameJetTouched;
    // Control x Fire, the firebrand: a flame at the aim that lights the
    // ground under it while the key is held (upkeep like the water hold).
    struct SpiritBrand {
        f32 heat { 1.0f };
        f32 radius { 1.0f };
        f32 costPeriod { 1.0f };
        f32 costClock { 0.0f };
        f32 upkeepScale { 0.25f };
        f32 pulseClock { 0.0f };
        u32 emitter { 0 };
        core::Guid ability;
    };
    std::optional<SpiritBrand> spiritBrand;
    void updateBrand(const SpiritContext& ctx, f32 dt);
    void endBrand(const SpiritContext& ctx);
    static constexpr f32 kBrandPulse = 0.25f;
    // Destroy x Fire on self, held: the fire ward — a douse repeated by
    // every fire job around the caster, who feels no flame meanwhile.
    struct SpiritGlobe {
        f32 radius { 6.0f };
        f32 costPeriod { 1.0f };
        f32 costClock { 0.0f };
        f32 upkeepScale { 0.25f };
        u32 emitter { 0 };
        core::Guid ability;
    };
    std::optional<SpiritGlobe> spiritGlobe;
    void updateGlobe(const SpiritContext& ctx, f32 dt);
    void endGlobe(const SpiritContext& ctx);
    // Create x Fire as a stream, held: the flame jet — the fire's
    // counterpart of the water jet, short and straight: flames stream
    // from the hand along the aim, the ground under the cone catches
    // (props and trees through the field), whoever stands in it burns.
    struct SpiritFlameJet {
        f32 heat { 1.0f };
        f32 radius { 1.0f };
        f32 range { 10.0f };
        f32 costPeriod { 0.5f };
        f32 costClock { 0.0f };
        f32 upkeepScale { 0.25f };
        f32 pulseClock { 0.0f };
        u32 emitter { 0 };
        core::Guid ability;
    };
    std::optional<SpiritFlameJet> spiritFlameJet;
    void updateFlameJet(const SpiritContext& ctx, f32 dt);
    void endFlameJet(const SpiritContext& ctx);
    static constexpr f32 kFlameJetPulse = 0.25f;
    static constexpr f32 kFlameJetSpeed = 14.0f;     // m/s, the flames' travel
    static constexpr f32 kFlameJetHalfAngle = 0.28f; // radians, the cone
    // Understand x Fire: the cell under the aim (cold / heating / burning /
    // burnt and regrowing), its fuel, the nearest front.
    void castFireReading(const SpiritContext& ctx, const Vec3& at, f32 seconds,
                         bool live);

    // Wind (SpiritWind.cpp). The field rebuilt every Play frame.
    render::terrain::WindField worldWind;
    void updateWind(const SpiritContext& ctx, f32 dt);
    // The breath (Create x Wind as a stream, held).
    struct SpiritBlow {
        f32 speed { 12.0f };
        f32 radius { 6.0f };
        f32 range { 20.0f };
        f32 costPeriod { 0.5f };
        f32 costClock { 0.0f };
        f32 upkeepScale { 0.25f };
        u32 emitter { 0 };
        core::Guid ability;
    };
    std::optional<SpiritBlow> spiritBlow;
    void updateBlow(const SpiritContext& ctx, f32 dt);
    void endBlow(const SpiritContext& ctx);
    // The weather's wind under a spell: calmed (Destroy x Wind on self,
    // held) or steered where the caster faces (Control x Wind on self,
    // held). The base is what the weather set before the spell.
    struct SpiritWindHold {
        bool calm { false };
        bool direct { false };
        // Released: the wind eases back to the weather's over ~1 s
        // before the hold ends (a recast cancels the release).
        bool releasing { false };
        f32 calmFactor { 1.0f };
        f32 currentDeg { 0.0f }; // the eased heading (direct)
        f32 costPeriod { 1.0f };
        f32 costClock { 0.0f };
        f32 upkeepScale { 0.25f };
        core::Guid ability;
    };
    std::optional<SpiritWindHold> spiritWindHold;
    f32 windBaseStrength { 1.0f };
    f32 windBaseDirectionDeg { 20.0f };
    static constexpr f32 kWindHoldEase = 0.33f; // s; ~95 % of the way in a second
    void endWindHold(const SpiritContext& ctx);
    void resetWind(const SpiritContext& ctx);

    // The readings (one HUD block for the three Understand spells).
    enum class ReadingKind : u8 { Water, Ground, Fire };
    ReadingKind spiritReadingKind { ReadingKind::Water };
    vector<str> spiritReading; // one line per HUD slot (kReadingLines)
    f32 spiritReadingSeconds { 0.0f };
    // A channeled reading follows the aim while Q is held and closes on
    // release (the timed one just fades).
    bool spiritReadingLive { false };
    static constexpr u32 kReadingLines = 6;
    void updateReading(const SpiritContext& ctx, f32 dt);

    // --- The fire lane --------------------------------------------------
    struct FireShared {
        core::ConcurrentQueue<world::FireJobOutput> done;
    };
    sptr<FireShared> fireShared { std::make_shared<FireShared>() };
    std::unique_ptr<render::terrain::FireGrid> fireGrid; // null while in flight
    bool fireInFlight { false };
    bool fireActive { false };
    bool fireWake { false };
    u32 fireEpoch { 0 };
    f32 fireAccum { 0.0f }; // sim seconds owed to the 10 Hz step
    vector<world::FireIgnition> fireIgnitions;
    vector<world::FireDouse> fireDouses;
    std::optional<world::FireDouse> fireWard;
    render::terrain::FireParams fireParams;
    world::TreeFireParams treeFireParams;
    render::terrain::WindField laneWind; // the copy the next job spreads with
    world::GroundProps groundProps {};
    vector<u8> fireMask;
    vector<u8> fireGlowMask;
    vector<u8> fireStateMask;
    std::array<core::Guid,
               static_cast<size_t>(render::terrain::SpiritKind::kCount)>
        contactFx {};
    std::array<f32, static_cast<size_t>(render::terrain::SpiritKind::kCount)>
        contactEvery {};
    std::array<f32, static_cast<size_t>(render::terrain::SpiritKind::kCount)>
        contactHurt {};
    render::terraingen::GridSpec fireMaskSpec;
    vector<Vec2> fireCenters;
    vector<render::terrain::FireSavedCell> fireCells; // the last landed job's
    vector<render::terrain::FireSavedCell> fireRestores; // a loaded save's, until the next job
    render::terrain::FireStats lastFireStats;
    f32 fireMs { 0.0f };
    static constexpr u32 kMaxStepsPerJob = 5;
};

} // namespace game
