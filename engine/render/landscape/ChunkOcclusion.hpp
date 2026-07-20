#pragma once

#include <unordered_map>
#include <unordered_set>

#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Defines.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"

namespace core {
class JobSystem;
}

namespace render {

// Height-horizon occlusion culling (brick 26, CPU stage). The frustum can't
// reject chunks hidden BEHIND a ridge; this can. From the camera position,
// march a fan of azimuth rays outward over the exact terrain height
// function, recording the running maximum elevation slope per distance
// ring. A chunk is occluded when the horizon accumulated strictly BEFORE it
// tops the slope to its highest drawable point (meshed maxY + prop
// headroom). Every approximation errs toward "keep drawing":
//   - target slope uses the chunk's NEAREST corner and its maxY + headroom,
//   - the horizon is taken one ring short of the chunk,
//   - the minimum horizon over the chunk's azimuth span (±1 ray) is used.
// The rebuild is a pure function of (params, camera, chunk-top table): it
// runs on a JobSystem worker when the camera strays far enough from the
// last rebuild point, and the main thread swaps the occluded set on arrival
// (worker/queue pattern of the streaming systems). Until a result lands,
// everything stays visible.
class ChunkOcclusion {
public:
    static constexpr u32 kRayCount = 180;      // 2° azimuth fan
    static constexpr f32 kRingStep = 32.0f;    // meters between samples
    static constexpr u32 kRingCount = 30;      // reach: 960 m (view ring)
    static constexpr f32 kPropHeadroom = 86.0f; // tallest scaled tree
                                                // (x8 realistic trees,
                                                // dev 2026-07-20)
    static constexpr f32 kRebuildDistance = 8.0f; // camera delta triggering

    // Snapshot handed to the worker. `chunkTops` maps chunk key
    // ((u32)cx << 32 | (u32)cz) to the chunk's meshed maxY.
    struct Input {
        TerrainParams params;
        Vec3 cameraPos {};
        std::unordered_map<u64, f32> chunkTops;
        u64 generation { 0 };
    };
    struct Result {
        u64 generation { 0 };
        std::unordered_set<u64> occluded;
    };

    void create(core::JobSystem& jobSystem);

    // Main thread, once per frame: drains finished worker results.
    void pump();

    // True when the camera strayed far enough from the last rebuild point
    // and no rebuild is in flight — the caller then gathers the chunk-top
    // table (a copy is only made in that case) and calls rebuild().
    bool wantsRebuild(const Vec3& cameraPos) const;
    void rebuild(const TerrainParams& params, const Vec3& cameraPos,
                 std::unordered_map<u64, f32> chunkTops);

    // Terrain seed changed / teleport: drop the set, invalidate in-flight.
    void invalidate();

    const std::unordered_set<u64>* occludedSet() const {
        return occluded.empty() ? nullptr : &occluded;
    }
    u32 occludedCount() const { return static_cast<u32>(occluded.size()); }

private:
    struct Shared {
        core::ConcurrentQueue<Result> results;
    };

    sptr<Shared> shared;
    core::JobSystem* jobs { nullptr };
    std::unordered_set<u64> occluded;
    u64 generation { 0 };
    bool inFlight { false };
    Vec3 lastRebuildPos { 1e9f, 1e9f, 1e9f };
};

// The pure rebuild — exposed for the headless doctest.
ChunkOcclusion::Result computeOcclusion(const ChunkOcclusion::Input& input);

} // namespace render
