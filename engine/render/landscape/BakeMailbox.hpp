#pragma once

#include <memory>

#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Defines.hpp"
#include "engine/core/Jobs.hpp"

namespace render {

// The worker-bake mailbox shared by the ring-baked maps (TerrainLightMap,
// TerrainShadeMap, MistMap, FarTerrain): at most one bake in flight,
// results crossing back on a concurrent queue drained on the main thread,
// and a generation stamp orphaning in-flight results across a
// destroy/recreate. The Baked payload must carry a `u64 gen` field —
// kick() stamps it, drain() checks it.
template <typename Baked>
class BakeMailbox {
public:
    void create(core::JobSystem& jobSystem) {
        jobs = &jobSystem;
        built = std::make_shared<core::ConcurrentQueue<Baked>>();
    }

    // Orphan in-flight bakes. The generation keeps counting across
    // resets so a result from before can never land after.
    void reset() {
        ++generation;
        inFlight = false;
        uploaded = false;
    }

    // Land finished current-generation bakes through `land(done)`;
    // stale generations are dropped.
    template <typename Land>
    void drain(Land&& land) {
        Baked done;
        while (built->tryPop(done)) {
            if (done.gen != generation) {
                continue;
            }
            land(done);
            inFlight = false;
            uploaded = true;
        }
    }

    bool busy() const { return inFlight; }
    bool ready() const { return uploaded; } // at least one bake landed

    // Run `bake(baked)` on a worker; the stamped result comes back
    // through drain() on a later frame.
    template <typename Bake>
    void kick(Bake&& bake) {
        inFlight = true;
        jobs->enqueue([queue = built, gen = generation,
                       bake = std::forward<Bake>(bake)] {
            Baked baked;
            baked.gen = gen;
            bake(baked);
            queue->push(std::move(baked));
        });
    }

private:
    core::JobSystem* jobs { nullptr };
    std::shared_ptr<core::ConcurrentQueue<Baked>> built;
    bool inFlight { false };
    bool uploaded { false };
    u64 generation { 0 };
};

} // namespace render
