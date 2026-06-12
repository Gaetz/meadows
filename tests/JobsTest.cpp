#include <atomic>

#include <doctest/doctest.h>

#include "engine/core/Jobs.hpp"

TEST_CASE("jobs: counter waits for a full batch") {
    core::JobSystem jobs;
    std::atomic<u64> sum { 0 };
    core::JobCounter counter;

    constexpr u32 jobCount = 1000;
    for (u32 i = 1; i <= jobCount; ++i) {
        jobs.enqueue(counter, [&sum, i] { sum += i; });
    }
    jobs.wait(counter);

    CHECK(sum == u64(jobCount) * (jobCount + 1) / 2);
    CHECK(counter.done());
}

TEST_CASE("jobs: destructor drains the queue") {
    std::atomic<u32> ran { 0 };
    constexpr u32 jobCount = 200;
    {
        core::JobSystem jobs { 2 };
        for (u32 i = 0; i < jobCount; ++i) {
            jobs.enqueue([&ran] { ran++; });
        }
        // No wait: destruction must finish everything already queued.
    }
    CHECK(ran == jobCount);
}

TEST_CASE("jobs: a counter is reusable across batches") {
    core::JobSystem jobs;
    core::JobCounter counter;
    std::atomic<u32> ran { 0 };
    for (u32 round = 0; round < 3; ++round) {
        for (u32 i = 0; i < 50; ++i) {
            jobs.enqueue(counter, [&ran] { ran++; });
        }
        jobs.wait(counter);
    }
    CHECK(ran == 150);
}
