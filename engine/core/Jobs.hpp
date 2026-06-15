#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

#include "engine/core/Defines.hpp"

namespace core {

// Wait-group for a batch of jobs. The counter must outlive its jobs: keep it
// on the stack only if you JobSystem::wait() on it before leaving scope.
class JobCounter {
public:
    bool done() const { return pending.load(std::memory_order_acquire) == 0; }

private:
    friend class JobSystem;

    std::atomic<u32> pending { 0 };
    std::mutex mutex;
    std::condition_variable cv;
};

// Simple thread pool: one shared queue, mutex + condvar (§10: simplest thing
// that exercises the concept). Work stealing, priorities, or fibers only when
// a real workload demands them; the first real client is async asset/cell
// streaming (Phase 8). The destructor drains queued jobs before joining.
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

private:
    void workerLoop();

    vector<std::thread> workers;
    std::queue<Job> jobs;
    std::mutex mutex;
    std::condition_variable cv;
    bool stopping { false };
};

} // namespace core
