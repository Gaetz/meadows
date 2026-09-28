#pragma once

#include <array>
#include <memory>
#include <optional>

#include "data/forms/FormDatabase.hpp"
#include "engine/core/ConcurrentQueue.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/terrain/SpiritField.hpp"
#include "engine/terrain/WaterBodies.hpp"
#include "engine/terrain/WaterSim.hpp"
#include "engine/terrain/generation/WaterSolve.hpp"
#include "world/spirit/SpiritFire.hpp"
#include "world/spirit/SpiritJets.hpp"
#include "world/spirit/SpiritRules.hpp"
#include "world/spirit/SpiritSources.hpp"

namespace core {
class JobSystem;
}

namespace game {

// The scene-side owner of the spirit framework (chantier ESPRITS): the
// placed sources, the transient jets, the compiled rule table, and the
// fire lane — the one spirit job in flight, distinct from the water's.
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
    vector<data::Record> capture() const { return sources.capture(); }
    const world::SpiritSourceList& list() const { return sources; }
    const world::SpiritRuleTable& rules() const { return table; }

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
    std::optional<render::terraingen::WaterSource> holdSource;
    EarthLift lift;

    struct FireShared {
        core::ConcurrentQueue<world::FireJobOutput> done;
    };
    sptr<FireShared> fireShared { std::make_shared<FireShared>() };
    std::unique_ptr<render::terrain::FireGrid> fireGrid; // null while in flight
    bool fireInFlight { false };
    bool fireActive { false };
    u32 fireEpoch { 0 };
    f32 fireAccum { 0.0f }; // sim seconds owed to the 10 Hz step
    vector<world::FireIgnition> fireIgnitions;
    vector<world::FireDouse> fireDouses;
    std::optional<world::FireDouse> fireWard;
    render::terrain::FireParams fireParams;
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
    render::terrain::FireStats lastFireStats;
    f32 fireMs { 0.0f };
    static constexpr u32 kMaxStepsPerJob = 5;
};

} // namespace game
