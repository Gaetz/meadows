#pragma once

#include <memory>

#include <glm/glm.hpp>

#include "engine/core/Clock.hpp"
#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Defines.hpp"
#include "engine/core/Jobs.hpp"

namespace core {

// A stamp that must hold still before a heavy reaction fires: a tile
// publish burst bumps the content stamp once per tile, the consumer
// rescans ONCE, after the burst has been quiet for `seconds`.
struct QuietStamp {
    u64 seen { 0 };
    TimePoint since {};
    // Feed the current stamp; true once it has not moved for `seconds`.
    bool settled(u64 stamp, f64 seconds) {
        if (stamp != seen) {
            seen = stamp;
            since = clockNow();
        }
        return secondsSince(since) > seconds;
    }
};

// The worker-bake mailbox shared by the ring-baked maps (TerrainLightMap,
// TerrainShadeMap, MistMap, FarTerrain): at most one bake in flight,
// results crossing back on a concurrent queue drained on the main thread,
// and a generation stamp orphaning in-flight results across a
// destroy/recreate. The Baked payload must carry a `u64 gen` field —
// kick() stamps it, drain() checks it.
template <typename Baked>
class BakeMailbox {
public:
    // `probeName` labels this map's bakes on the F6 worker-jobs table —
    // a static string literal (JobProbe contract).
    void create(core::JobSystem& jobSystem, const char* probeName) {
        jobs = &jobSystem;
        name = probeName;
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
    // The ring-baked maps' re-bake trigger: nothing landed yet, or the
    // focus left `reach` of the landed center.
    bool strayed(const Vec2& center, const Vec2& want, f32 reach) const {
        return !uploaded || glm::distance(want, center) > reach;
    }

    // Run `bake(baked, stop)` on a worker; the stamped result comes back
    // through drain() on a later frame. `stop` is the JobSystem's shutdown
    // flag: these bakes are minutes of height()-sampling, and ~JobSystem
    // drains the queue then joins — a bake that never polls keeps the
    // process alive long after the window closed. Heavy texel loops must
    // poll it per row and bail; a stopped bake's partial result is
    // dropped here, never pushed.
    template <typename Bake>
    void kick(Bake&& bake) {
        inFlight = true;
        jobs->enqueue([queue = built, gen = generation, jobs = jobs,
                       probeName = name,
                       bake = std::forward<Bake>(bake)]() mutable {
            if (jobs->isStopping()) {
                return; // queued behind shutdown: never start
            }
            core::JobProbe::Scope probe { &jobs->probe(), probeName };
            Baked baked;
            baked.gen = gen;
            bake(baked, jobs->stopFlag());
            if (jobs->isStopping()) {
                return; // partial bake: never lands
            }
            queue->push(std::move(baked));
        });
    }

private:
    core::JobSystem* jobs { nullptr };
    const char* name { "bake" };
    std::shared_ptr<core::ConcurrentQueue<Baked>> built;
    bool inFlight { false };
    bool uploaded { false };
    u64 generation { 0 };
};

} // namespace core
