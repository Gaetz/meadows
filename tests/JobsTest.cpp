// Job system smoke test: grouped jobs + counter wait, then destructor drain.
// Plain checks (no framework yet); a real test framework lands with the
// Phase-1 data-model tests, where testing becomes mandatory (§8).

#include <atomic>

#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"

namespace {

bool testCounterWait() {
    core::JobSystem jobs;
    std::atomic<u64> sum { 0 };
    core::JobCounter counter;

    constexpr u32 jobCount = 1000;
    for (u32 i = 1; i <= jobCount; ++i) {
        jobs.enqueue(counter, [&sum, i] { sum += i; });
    }
    jobs.wait(counter);

    constexpr u64 expected = u64(jobCount) * (jobCount + 1) / 2;
    if (sum != expected || !counter.done()) {
        LOG_ERROR("counter wait: sum = {}, expected {}", sum.load(), expected);
        return false;
    }
    return true;
}

bool testDestructorDrains() {
    std::atomic<u32> ran { 0 };
    constexpr u32 jobCount = 200;
    {
        core::JobSystem jobs { 2 };
        for (u32 i = 0; i < jobCount; ++i) {
            jobs.enqueue([&ran] { ran++; });
        }
        // No wait: destruction must finish everything already queued.
    }
    if (ran != jobCount) {
        LOG_ERROR("destructor drain: {} of {} jobs ran", ran.load(), jobCount);
        return false;
    }
    return true;
}

bool testReuseCounter() {
    core::JobSystem jobs;
    core::JobCounter counter;
    std::atomic<u32> ran { 0 };
    for (u32 round = 0; round < 3; ++round) {
        for (u32 i = 0; i < 50; ++i) {
            jobs.enqueue(counter, [&ran] { ran++; });
        }
        jobs.wait(counter);
    }
    if (ran != 150) {
        LOG_ERROR("counter reuse: {} of 150 jobs ran", ran.load());
        return false;
    }
    return true;
}

} // namespace

int main() {
    core::Log::init();
    bool ok = true;
    ok &= testCounterWait();
    ok &= testDestructorDrains();
    ok &= testReuseCounter();
    LOG_INFO("jobs test: {}", ok ? "PASSED" : "FAILED");
    return ok ? 0 : 1;
}
