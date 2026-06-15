#include <atomic>
#include <thread>

#include <doctest/doctest.h>

#include "engine/core/ConcurrentQueue.hpp"

TEST_CASE("concurrent queue: single-thread FIFO push / tryPop") {
    core::ConcurrentQueue<int> q;
    CHECK(q.empty());

    q.push(1);
    q.push(2);
    q.push(3);
    CHECK(q.size() == 3);

    int v = 0;
    CHECK(q.tryPop(v));
    CHECK(v == 1);
    CHECK(q.tryPop(v));
    CHECK(v == 2);
    CHECK(q.tryPop(v));
    CHECK(v == 3);
    CHECK_FALSE(q.tryPop(v)); // empty: returns false, leaves v untouched
    CHECK(q.empty());
}

TEST_CASE("concurrent queue: drain takes everything in FIFO order then empties") {
    core::ConcurrentQueue<int> q;
    for (int i = 0; i < 5; ++i) {
        q.push(i);
    }

    vector<int> seen;
    const u32 n = q.drain([&](int&& x) { seen.push_back(x); });

    CHECK(n == 5);
    CHECK(q.empty());
    CHECK(seen == vector<int> { 0, 1, 2, 3, 4 });

    // Draining an empty queue is a no-op.
    CHECK(q.drain([](int&&) {}) == 0);
}

TEST_CASE("concurrent queue: a callback may re-push without deadlocking") {
    core::ConcurrentQueue<int> q;
    q.push(0);
    q.push(1);

    // fn runs outside the lock, so pushing back in is safe (here: requeue once).
    vector<int> seen;
    q.drain([&](int&& x) {
        seen.push_back(x);
        if (x == 1) {
            q.push(99);
        }
    });
    CHECK(seen == vector<int> { 0, 1 });
    // The re-pushed item lands in the next drain, not this one.
    int v = 0;
    CHECK(q.tryPop(v));
    CHECK(v == 99);
}

TEST_CASE("concurrent queue: many producers, one consumer, nothing lost") {
    core::ConcurrentQueue<u32> q;
    constexpr u32 producers = 8;
    constexpr u32 perProducer = 10'000;
    constexpr u32 expected = producers * perProducer;

    std::atomic<bool> go { false };
    vector<std::thread> threads;
    for (u32 p = 0; p < producers; ++p) {
        threads.emplace_back([&q, &go] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (u32 i = 0; i < perProducer; ++i) {
                q.push(1);
            }
        });
    }

    go.store(true, std::memory_order_release);

    // Consumer drains repeatedly until it has accounted for every push, exactly
    // as the frame thread would across frames.
    u64 received = 0;
    u64 sum = 0;
    while (received < expected) {
        received += q.drain([&](u32&& x) { sum += x; });
    }

    for (std::thread& t : threads) {
        t.join();
    }

    CHECK(received == expected);
    CHECK(sum == expected); // every push contributed exactly 1 — none lost/dup'd
    CHECK(q.empty());
}
