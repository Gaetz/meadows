#pragma once

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "engine/core/Clock.hpp"
#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Defines.hpp"
#include "engine/core/Jobs.hpp"

namespace render {

// The shared terrain chunk grid key (audit U3-4): (cx, cz) packed into a
// u64, matching HeightPatches::keyOf and the occluders' convention.
inline constexpr u64 chunkKey(i32 cx, i32 cz) {
    return (static_cast<u64>(static_cast<u32>(cx)) << 32) |
           static_cast<u32>(cz);
}
inline constexpr i32 chunkKeyCx(u64 key) {
    return static_cast<i32>(key >> 32);
}
inline constexpr i32 chunkKeyCz(u64 key) {
    return static_cast<i32>(key & 0xffffffffu);
}
inline i32 chunkCoordOf(f32 worldCoord, f32 chunkSize) {
    return static_cast<i32>(std::floor(worldCoord / chunkSize));
}

// The chunk-streaming ring shared by Terrain/Grass/Vegetation (audit U3-1):
// a map of chunks keyed on the terrain grid, worker builds coming back
// through a generation-stamped ConcurrentQueue, budgeted nearest-first
// requests, and evict-beyond-hysteresis. The systems keep what actually
// differs — the worker payload, the GPU upload, the want/evict rules —
// as small lambdas; the ring mechanics live here once.
//
// Threading contract (Phase 5): every method runs on the MAIN thread;
// only the queue is worker-fed. `Shared` is owned via shared_ptr and
// captured by every job, so a job that outlives the system still has a
// valid queue to push into (the TextureCache teardown-safety pattern).
// Stale results (generation bumped by invalidateAll) are dropped in pump.
template <typename Chunk, typename Payload>
class ChunkStreamer {
public:
    struct Built {
        i32 cx { 0 };
        i32 cz { 0 };
        u64 generation { 0 };
        Payload payload {};
    };

    void create(core::JobSystem& jobSystem) {
        jobs = &jobSystem;
        shared = std::make_shared<Shared>();
    }

    // Bumps the generation (in-flight results die on arrival) and drops
    // every chunk after `drop(chunk)` frees what it owns. Serves destroy()
    // and regenerate() alike; create() state (jobs/queue) survives.
    template <typename Drop>
    void invalidateAll(Drop&& drop) {
        ++generation_;
        for (auto& [key, chunk] : chunks) {
            drop(chunk);
        }
        chunks.clear();
    }

    // Worker-side build: runs `build()` on a JobSystem worker and pushes
    // the generation-stamped result. `build` must capture everything it
    // reads BY VALUE (it outlives the frame).
    template <typename BuildFn>
    void enqueueBuild(i32 cx, i32 cz, BuildFn&& build) {
        jobs->enqueue([sharedRef = shared, cx, cz, gen = generation_,
                       fn = std::forward<BuildFn>(build)] {
            sharedRef->built.push({ cx, cz, gen, fn() });
        });
    }

    // Budgeted upload pump: drains finished builds, drops stale
    // generations, hands each live result to `accept(key, built)` which
    // does the system-specific guards + GPU upload and returns true when
    // it consumed an upload slot. `msBudget` <= 0 disables the time cap;
    // with a cap, at least one upload always lands (progress guarantee).
    template <typename Accept>
    u32 pump(u32 maxUploads, f64 msBudget, Accept&& accept) {
        u32 uploads = 0;
        const auto start = core::clockNow();
        Built built;
        while (uploads < maxUploads &&
               (msBudget <= 0.0 || uploads == 0 ||
                core::millisecondsSince(start) < msBudget) &&
               shared->built.tryPop(built)) {
            if (built.generation != generation_) {
                continue; // stale: regenerated or torn down since the request
            }
            if (accept(chunkKey(built.cx, built.cz), built)) {
                ++uploads;
            }
        }
        return uploads;
    }

    // Nearest-first budgeted requests: scans the (2r+1)² ring around the
    // camera chunk, collects cells where `want(cx, cz, dx, dz)` says so,
    // sorts center-out and fires `request(cx, cz, dx, dz)` for the first
    // `maxRequests`. Anything past the budget is simply re-detected next
    // frame — the state IS the queue.
    template <typename Want, typename Request>
    void requestMissing(i32 camCx, i32 camCz, i32 viewRadius,
                        u32 maxRequests, Want&& want, Request&& request) {
        struct Candidate {
            i32 cx, cz, dist2, dx, dz;
        };
        vector<Candidate> wanted;
        for (i32 dz = -viewRadius; dz <= viewRadius; ++dz) {
            for (i32 dx = -viewRadius; dx <= viewRadius; ++dx) {
                const i32 cx = camCx + dx;
                const i32 cz = camCz + dz;
                if (want(cx, cz, dx, dz)) {
                    wanted.push_back({ cx, cz, dx * dx + dz * dz, dx, dz });
                }
            }
        }
        std::sort(wanted.begin(), wanted.end(),
                  [](const Candidate& a, const Candidate& b) {
                      return a.dist2 < b.dist2;
                  });
        u32 requests = 0;
        for (const Candidate& c : wanted) {
            if (requests >= maxRequests) {
                break;
            }
            request(c.cx, c.cz, c.dx, c.dz);
            ++requests;
        }
    }

    // Hysteresis eviction: chunks beyond `evictRadius` (Chebyshev) hand
    // their GPU state to `evict(chunk)` and leave the map.
    template <typename Evict>
    void evictFar(i32 camCx, i32 camCz, i32 evictRadius, Evict&& evict) {
        for (auto it = chunks.begin(); it != chunks.end();) {
            const i32 dist =
                std::max(std::abs(chunkKeyCx(it->first) - camCx),
                         std::abs(chunkKeyCz(it->first) - camCz));
            if (dist <= evictRadius) {
                ++it;
                continue;
            }
            evict(it->second);
            it = chunks.erase(it);
        }
    }

    u64 generation() const { return generation_; }

    // The chunk map stays directly accessible: the systems' draw loops,
    // stats and invalidate paths iterate it every frame.
    std::unordered_map<u64, Chunk> chunks;

private:
    struct Shared {
        core::ConcurrentQueue<Built> built;
    };

    sptr<Shared> shared;
    core::JobSystem* jobs { nullptr };
    u64 generation_ { 0 };
};

} // namespace render
