#pragma once

#include <algorithm>
#include <unordered_map>

#include "engine/core/Clock.hpp"
#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Defines.hpp"

namespace core {

// The worker half of the perf probes (GpuProbe = GPU passes, FrameProbe =
// main-thread sections): named wall-clock costs of JobSystem jobs, folded
// into per-name rows the frame thread reads for the F6 table. Workers
// record; the frame thread drains — the standard completion-queue seam
// (docs/PHASE-5.md), so recording never blocks a worker and reading never
// blocks the frame.
//
// Names must be STATIC string literals: rows key on the pointer, and a
// worker may record after the naming call site returned. Instrumentation
// is opt-in per job body (a blanket JobSystem::enqueue wrap would time
// unnamed asset noise) — wrap the payload in a JobProbe::Scope.
class JobProbe {
public:
    // RAII: measures the enclosed job body. Null-tolerant so call sites
    // without a JobSystem (headless tools, tests) skip instrumentation.
    class Scope {
    public:
        Scope(JobProbe* probe, const char* name)
            : probe { probe }, name { name }, start { clockNow() } {}
        ~Scope() {
            if (probe) {
                probe->record(name, millisecondsSince(start));
            }
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        JobProbe* probe;
        const char* name;
        TimePoint start;
    };

    // Any thread.
    void record(const char* name, f64 ms) { entries.push({ name, ms }); }

    // Per-name stats over the current window (since the last reset).
    struct Row {
        const char* name { nullptr };
        u32 runs { 0 };
        f64 lastMs { 0.0 };
        f64 maxMs { 0.0 };
        f64 totalMs { 0.0 };
        f64 averageMs() const {
            return runs > 0 ? totalMs / static_cast<f64>(runs) : 0.0;
        }
    };

    // Frame thread only: fold queued records into the rows. Cheap when
    // nothing landed. Call once per frame before reading rows().
    void drain() {
        entries.drain([this](Entry&& e) {
            const auto [it, inserted] = index.try_emplace(
                e.name, static_cast<u32>(rowsCache.size()));
            if (inserted) {
                rowsCache.push_back({ e.name });
            }
            Row& row = rowsCache[it->second];
            ++row.runs;
            row.lastMs = e.ms;
            row.maxMs = std::max(row.maxMs, e.ms);
            row.totalMs += e.ms;
        });
    }

    // Rows in first-seen order (stable across the window).
    const vector<Row>& rows() const { return rowsCache; }

    f64 windowTotalMs() const {
        f64 total = 0.0;
        for (const Row& row : rowsCache) {
            total += row.totalMs;
        }
        return total;
    }

    void resetWindow() {
        rowsCache.clear();
        index.clear();
    }

private:
    struct Entry {
        const char* name;
        f64 ms;
    };

    ConcurrentQueue<Entry> entries;
    // Keyed on the literal's pointer — see the class contract.
    std::unordered_map<const char*, u32> index;
    vector<Row> rowsCache;
};

} // namespace core
