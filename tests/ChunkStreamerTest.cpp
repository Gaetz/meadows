#include <doctest/doctest.h>

#include "engine/core/Jobs.hpp"
#include "engine/render/landscape/ChunkStreamer.hpp"

// The shared chunk-streaming ring (audit U3-1) — extracted from
// Terrain/Grass/Vegetation precisely so its mechanics get locked headless:
// key packing, nearest-first budgeted requests, hysteresis eviction, and
// the generation-stamped worker queue (stale results die on arrival).

namespace {

struct TestChunk {
    bool resident { false };
    int value { 0 };
};

using Streamer = render::ChunkStreamer<TestChunk, int>;

} // namespace

TEST_CASE("chunk streamer: key packing round-trips negative coords") {
    const u64 key = render::chunkKey(-7, 13);
    CHECK(render::chunkKeyCx(key) == -7);
    CHECK(render::chunkKeyCz(key) == 13);
    const u64 negBoth = render::chunkKey(-1, -1);
    CHECK(render::chunkKeyCx(negBoth) == -1);
    CHECK(render::chunkKeyCz(negBoth) == -1);
    CHECK(render::chunkCoordOf(-0.5f, 64.0f) == -1);
    CHECK(render::chunkCoordOf(63.9f, 64.0f) == 0);
    CHECK(render::chunkCoordOf(64.0f, 64.0f) == 1);
}

TEST_CASE("chunk streamer: requests go out nearest-first, budgeted") {
    Streamer streamer;
    core::JobSystem jobs { 1 };
    streamer.create(jobs);

    vector<std::pair<i32, i32>> requested;
    streamer.requestMissing(
        10, 20, /*viewRadius=*/2, /*maxRequests=*/3,
        [](i32, i32, i32, i32) { return true; }, // everything is missing
        [&](i32 cx, i32 cz, i32, i32) { requested.push_back({ cx, cz }); });

    REQUIRE(requested.size() == 3); // the budget, not the (2r+1)^2 ring
    CHECK(requested[0] == std::pair<i32, i32> { 10, 20 }); // center first
    // The next two are ring-1 cells (dist2 == 1 or 2).
    for (size_t i = 1; i < requested.size(); ++i) {
        const i32 dx = requested[i].first - 10;
        const i32 dz = requested[i].second - 20;
        CHECK(dx * dx + dz * dz <= 2);
    }
}

TEST_CASE("chunk streamer: pump respects the upload budget") {
    Streamer streamer;
    {
        core::JobSystem jobs { 1 };
        streamer.create(jobs);
        for (i32 i = 0; i < 5; ++i) {
            streamer.enqueueBuild(i, 0, [i] { return i * 10; });
        }
    } // JobSystem destruction drains the queue: all 5 results are in.

    u32 accepted = 0;
    const auto acceptAll = [&](u64, Streamer::Built& built) {
        CHECK(built.payload == built.cx * 10);
        ++accepted;
        return true;
    };
    CHECK(streamer.pump(2, 0.0, acceptAll) == 2);
    CHECK(streamer.pump(2, 0.0, acceptAll) == 2);
    CHECK(streamer.pump(2, 0.0, acceptAll) == 1); // queue exhausted
    CHECK(accepted == 5);
}

TEST_CASE("chunk streamer: a rejected result frees its upload slot") {
    Streamer streamer;
    {
        core::JobSystem jobs { 1 };
        streamer.create(jobs);
        for (i32 i = 0; i < 3; ++i) {
            streamer.enqueueBuild(i, 0, [] { return 0; });
        }
    }
    // Accept only the last one: the two rejections must not count against
    // the budget of 1 (evicted-while-in-flight results are free).
    u32 accepted = 0;
    streamer.pump(1, 0.0, [&](u64 key, Streamer::Built&) {
        if (render::chunkKeyCx(key) != 2) {
            return false;
        }
        ++accepted;
        return true;
    });
    CHECK(accepted == 1);
}

TEST_CASE("chunk streamer: stale generations are dropped on arrival") {
    Streamer streamer;
    {
        core::JobSystem jobs { 1 };
        streamer.create(jobs);
        streamer.enqueueBuild(3, -2, [] { return 42; });
    } // result (generation 0) sits in the queue

    // Regenerate: everything in flight belongs to the previous world.
    streamer.invalidateAll([](TestChunk&) {});
    u32 accepted = 0;
    streamer.pump(8, 0.0, [&](u64, Streamer::Built&) {
        ++accepted;
        return true;
    });
    CHECK(accepted == 0);
}

TEST_CASE("chunk streamer: eviction honors the hysteresis radius") {
    Streamer streamer;
    core::JobSystem jobs { 1 };
    streamer.create(jobs);

    streamer.chunks.emplace(render::chunkKey(0, 0), TestChunk { true, 1 });
    streamer.chunks.emplace(render::chunkKey(2, 0), TestChunk { true, 2 });
    streamer.chunks.emplace(render::chunkKey(0, 3), TestChunk { true, 3 });
    streamer.chunks.emplace(render::chunkKey(-4, 1), TestChunk { true, 4 });

    vector<int> evicted;
    streamer.evictFar(0, 0, /*evictRadius=*/2, [&](TestChunk& chunk) {
        evicted.push_back(chunk.value);
    });

    // Chebyshev distance: (0,0)=0 and (2,0)=2 stay; (0,3)=3 and (-4,1)=4 go.
    CHECK(streamer.chunks.contains(render::chunkKey(0, 0)));
    CHECK(streamer.chunks.contains(render::chunkKey(2, 0)));
    CHECK(streamer.chunks.size() == 2);
    REQUIRE(evicted.size() == 2);
    CHECK((evicted[0] + evicted[1]) == 7); // values 3 and 4, any order
}

TEST_CASE("chunk streamer: round trip request -> worker -> pump") {
    Streamer streamer;
    {
        core::JobSystem jobs { 1 };
        streamer.create(jobs);
        streamer.requestMissing(
            0, 0, 1, 99,
            [&](i32 cx, i32 cz, i32, i32) {
                return !streamer.chunks.contains(render::chunkKey(cx, cz));
            },
            [&](i32 cx, i32 cz, i32, i32) {
                streamer.chunks.emplace(render::chunkKey(cx, cz),
                                        TestChunk {});
                streamer.enqueueBuild(cx, cz, [cx, cz] { return cx + cz; });
            });
        CHECK(streamer.chunks.size() == 9); // the full 3x3 ring
    }

    u32 accepted = 0;
    while (streamer.pump(4, 0.0, [&](u64 key, Streamer::Built& built) {
        auto& chunk = streamer.chunks.at(key);
        CHECK_FALSE(chunk.resident);
        chunk.resident = true;
        chunk.value = built.payload;
        ++accepted;
        return true;
    }) > 0) {
    }
    CHECK(accepted == 9);
    CHECK(streamer.chunks.at(render::chunkKey(1, -1)).value == 0);
    CHECK(streamer.chunks.at(render::chunkKey(1, 1)).value == 2);
}
