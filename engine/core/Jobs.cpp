#include "engine/core/Jobs.hpp"

#include "engine/core/Log.hpp"

namespace core {

JobSystem::JobSystem(u32 threadCount) {
    u32 count = threadCount;
    if (count == 0) {
        const u32 hardware = std::thread::hardware_concurrency();
        count = hardware > 1 ? hardware - 1 : 1;
    }
    workers.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        workers.emplace_back([this] { workerLoop(); });
    }
    LOG_INFO("Job system: {} workers", count);
}

JobSystem::~JobSystem() {
    {
        std::lock_guard lock { mutex };
        stopping = true;
    }
    cv.notify_all();
    for (std::thread& worker : workers) {
        worker.join();
    }
}

void JobSystem::workerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock { mutex };
            cv.wait(lock, [this] { return stopping || !jobs.empty(); });
            if (jobs.empty()) {
                return; // stopping and drained
            }
            job = std::move(jobs.front());
            jobs.pop();
        }
        job();
    }
}

void JobSystem::enqueue(Job job) {
    {
        std::lock_guard lock { mutex };
        jobs.push(std::move(job));
    }
    cv.notify_one();
}

void JobSystem::enqueue(JobCounter& counter, Job job) {
    counter.pending.fetch_add(1, std::memory_order_relaxed);
    enqueue([&counter, job = std::move(job)] {
        job();
        if (counter.pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            // Last job: take the lock so a waiter between its predicate check
            // and its wait cannot miss the notify.
            { std::lock_guard lock { counter.mutex }; }
            counter.cv.notify_all();
        }
    });
}

void JobSystem::wait(JobCounter& counter) {
    std::unique_lock lock { counter.mutex };
    counter.cv.wait(lock, [&counter] { return counter.done(); });
}

} // namespace core
