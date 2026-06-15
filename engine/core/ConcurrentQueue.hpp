#pragma once

#include <mutex>
#include <queue>
#include <utility>

#include "engine/core/Defines.hpp"

namespace core {

// A non-blocking mailbox for handing results from worker threads back to a
// single consumer (the main/frame thread) — the completion-queue primitive of
// the threading model (CLAUDE.md §9 Phase 4.5). Many producers push; the
// consumer drains whatever is present and moves on.
//
// It NEVER blocks the consumer — that is the whole point, and what sets it
// apart from JobSystem. `JobSystem::wait` stalls the caller until a batch
// finishes; draining this queue stalls the frame on nothing. You apply the
// results that happen to be ready this frame and pick the rest up next frame.
// So a streaming/asset job pushes its result here, and a main-thread system
// drains it at a fixed point each frame (spawn refs, upload GPU, flip handles).
//
// MPSC by intent (multi-producer, single-consumer). Mutex-guarded — the
// simplest thing that exercises the concept (§10); a lock-free ring buffer is
// only worth it if profiling ever shows this lock is hot.
//
// Ordering: FIFO, but cross-producer interleaving is timing-dependent.
// Deterministic application (saves/replays, §8) is the CONSUMER's job: collect
// a frame's batch, then sort it (e.g. by GUID) before applying.
template<class T>
class ConcurrentQueue {
public:
    ConcurrentQueue() = default;
    ConcurrentQueue(const ConcurrentQueue&) = delete;
    ConcurrentQueue& operator=(const ConcurrentQueue&) = delete;

    // Any thread. Hand a result to the consumer.
    void push(T value) {
        std::lock_guard lock { mutex };
        items.push(std::move(value));
    }

    // Consumer only. Pops one item if present; returns false if empty. Never
    // blocks.
    bool tryPop(T& out) {
        std::lock_guard lock { mutex };
        if (items.empty()) {
            return false;
        }
        out = std::move(items.front());
        items.pop();
        return true;
    }

    // Consumer only. Atomically takes everything queued right now and invokes
    // fn(T&&) for each in FIFO order, OUTSIDE the lock — so producers keep
    // running while results are applied, and fn may itself push without
    // deadlocking. Returns the number of items processed.
    template<class Fn>
    u32 drain(Fn&& fn) {
        std::queue<T> batch;
        {
            std::lock_guard lock { mutex };
            std::swap(batch, items);
        }
        u32 count = 0;
        while (!batch.empty()) {
            fn(std::move(batch.front()));
            batch.pop();
            ++count;
        }
        return count;
    }

    bool empty() const {
        std::lock_guard lock { mutex };
        return items.empty();
    }

    u32 size() const {
        std::lock_guard lock { mutex };
        return static_cast<u32>(items.size());
    }

private:
    mutable std::mutex mutex;
    std::queue<T> items;
};

} // namespace core
