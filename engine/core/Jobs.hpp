#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

#include "engine/core/Assert.hpp"
#include "engine/core/Defines.hpp"

namespace core {

// Wait-group for a batch of jobs. The counter must outlive its jobs: keep it
// on the stack only if you JobSystem::wait() on it before leaving scope.
class JobCounter {
public:
    // Lifetime contract, enforced at runtime:
    // destroying a counter while enqueued jobs still reference it IS the
    // dangling-reference UB. Catch it at the source, not as a corrupted
    // atomic later. Non-copyable/movable: workers hold its address.
    ~JobCounter() {
        ENGINE_ASSERT_MSG(done(),
                          "JobCounter destroyed with jobs in flight — "
                          "JobSystem::wait() on it before leaving scope");
    }
    JobCounter() = default;
    JobCounter(const JobCounter&) = delete;
    JobCounter& operator=(const JobCounter&) = delete;

    bool done() const { return pending.load(std::memory_order_acquire) == 0; }

private:
    friend class JobSystem;

    std::atomic<u32> pending { 0 };
    std::mutex mutex;
    std::condition_variable cv;
};

// Simple thread pool: one shared queue, mutex + condvar (§10: simplest thing
// that exercises the concept). Work stealing, priorities, or fibers only when
// a real workload demands them; the main client is async asset/cell
// streaming. The destructor drains queued jobs before joining.
class JobSystem {
public:
    using Job = std::function<void()>;

    // threadCount 0 = one worker per hardware thread, minus the main thread.
    explicit JobSystem(u32 threadCount = 0);
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // Fire and forget.
    void enqueue(Job job);

    // Grouped: `counter` tracks completion of every job enqueued on it.
    void enqueue(JobCounter& counter, Job job);

    // Blocks the calling thread until the counter's jobs are all done.
    void wait(JobCounter& counter);

    u32 workerCount() const { return static_cast<u32>(workers.size()); }

    // True once shutdown began. The destructor DRAINS the queue (a
    // queued save must still land), so every EXPENSIVE, abandonable
    // job (tile bakes, sim pre-rolls) must poll this and bail early —
    // quitting mid-bake otherwise kept the process alive for minutes
    // of full-speed baking behind a closed window (measured).
    bool isStopping() const {
        return stopRequested.load(std::memory_order_relaxed);
    }

    // Raise the stop flag WITHOUT joining: the frame loop calls this
    // the moment it ends, so jobs already RUNNING (a minutes-long
    // erosion bake) see the cancellation while scene teardown is
    // still going — waiting for ~JobSystem to raise it left them
    // running to completion first (the "game never closes" hang).
    void requestStop() {
        stopRequested.store(true, std::memory_order_relaxed);
    }

    // The flag itself, for long-running kernels that poll a raw
    // atomic in their hot loops (terrain erosion, sim bursts).
    const std::atomic<bool>& stopFlag() const { return stopRequested; }

private:
    void workerLoop();

    vector<std::thread> workers;
    std::queue<Job> jobs;
    std::mutex mutex;
    std::condition_variable cv;
    bool stopping { false };
    std::atomic<bool> stopRequested { false };
};

} // namespace core
