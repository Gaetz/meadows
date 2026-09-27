#pragma once

#include "data/forms/FormDatabase.hpp"
#include "data/plugins/Record.hpp"
#include "engine/terrain/SpiritField.hpp"
#include "engine/terrain/generation/WaterSolve.hpp"
#include "world/worldspace/WorldForms.hpp"

// Chantier ESPRITS: the headless list of placed spirit sources — the
// runtime twin of the SpiritSourceForm records. Owned by the scene's
// SpiritDirector; mutated on the main thread only (the kernels get
// VALUE copies at job enqueue). Capacity-bounded so a spamming caster
// cannot flood a worker: the oldest source yields.

namespace world {

class SpiritSourceList {
public:
    static constexpr size_t kMaxSources = 16;

    // Places a source: mints its guid (deterministic — the sequence and
    // the spot), evicts the OLDEST when full. Returns the id.
    core::Guid add(render::terrain::SpiritSource source,
                   const core::Guid& worldspace);
    bool remove(const core::Guid& id);
    void clear();

    // Advances every finite lifetime by `simSeconds`; expired sources
    // leave. True when the set changed (the caller re-pushes to the
    // kernels only then).
    bool tick(f32 simSeconds);

    // The kernel view of the WATER sources on one map: a source of
    // radius <= 4 m is one disc; wider ones become a ring of seven (the
    // kernel's WaterSource stays {x, z, discharge} — no format bump).
    vector<render::terraingen::WaterSource> waterSourcesFor(
        const WorldspaceFilter& filter) const;

    // Save mirror: one SpiritSourceForm record per live source (sorted
    // by sequence — deterministic §8; expired ones never emitted).
    vector<data::Record> capture() const;
    // Rebuild from the resolved database (base plugins AND the save
    // layer alike — an authored spring is the same record).
    void apply(const data::FormDatabase& forms);

    struct Entry {
        render::terrain::SpiritSource source;
        core::Guid worldspace;
    };
    const vector<Entry>& entries() const { return list; }

private:
    vector<Entry> list;
    i32 nextSequence { 1 };
};

} // namespace world
