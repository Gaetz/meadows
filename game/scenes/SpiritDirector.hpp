#pragma once

#include <array>

#include "data/forms/FormDatabase.hpp"
#include "engine/terrain/SpiritField.hpp"
#include "engine/terrain/generation/WaterSolve.hpp"
#include "world/spirit/SpiritJets.hpp"
#include "world/spirit/SpiritRules.hpp"
#include "world/spirit/SpiritSources.hpp"

namespace game {

// The scene-side owner of the spirit framework (chantier ESPRITS): the
// placed sources, the transient jets, the compiled rule table, and — from
// the fire brick on — the one spirit job in flight. Main thread only; the
// water kernel gets VALUE copies of the sources at its own job enqueue.
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

    // The kernel view of one map: placed springs + every landed jet.
    vector<render::terraingen::WaterSource> waterSources(
        const core::Guid& worldspace) const {
        vector<render::terraingen::WaterSource> out =
            sources.waterSourcesFor(world::WorldspaceFilter { worldspace });
        jets.appendWaterSources(out);
        return out;
    }
    vector<data::Record> capture() const { return sources.capture(); }
    const world::SpiritSourceList& list() const { return sources; }
    const world::SpiritRuleTable& rules() const { return table; }

private:
    world::SpiritSourceList sources;
    world::SpiritJetList jets;
    world::SpiritRuleTable table;
    std::array<core::Guid,
               static_cast<size_t>(render::terrain::SpiritKind::kCount)>
        jetFx {};
};

} // namespace game
