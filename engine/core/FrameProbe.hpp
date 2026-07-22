#pragma once

#include <string>

#include "engine/core/Clock.hpp"
#include "engine/core/Defines.hpp"
#include "engine/core/Log.hpp"

namespace core {

// Frame spike breakdown (dev tool). Wrap the frame's suspect blocks in
// Scopes; endFrame() logs one WARN line with the per-block costs whenever
// the frame total crosses the threshold — so a stutter session leaves a
// log that names the culprit instead of a guess. Overhead when quiet:
// a dozen steady_clock reads per frame, no allocation after warm-up.
//
// If the logged blocks sum well below the frame total, the spike lives
// OUTSIDE the probes (present/swap, driver, OS) — that is a finding too.
class FrameProbe {
public:
    void beginFrame() {
        entries.clear();
        frameStart = clockNow();
    }

    class Scope {
    public:
        Scope(FrameProbe& probe, const char* name)
            : probe { probe }, name { name }, start { clockNow() } {}
        ~Scope() {
            probe.entries.push_back({ name, millisecondsSince(start) });
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        FrameProbe& probe;
        const char* name;
        TimePoint start;
    };

    // Logs the breakdown when the frame exceeded `thresholdMs`. Blocks
    // under 0.5 ms are elided (noise); `probed` = their sum, so a gap
    // between it and the total points outside the instrumented blocks.
    // Debug builds stay silent: everything is 10-30× slower there
    // (MSVC iterator debugging, unoptimized noise/Jolt), so every frame
    // "spikes" and the log drowns — profile stutters in Release.
    void endFrame(f64 thresholdMs = 25.0) {
#ifdef NDEBUG
        constexpr bool kSpikeLogging = true;
#else
        constexpr bool kSpikeLogging = false;
#endif
        if constexpr (!kSpikeLogging) {
            return;
        }
        const f64 total = millisecondsSince(frameStart);
        if (total < thresholdMs) {
            return;
        }
        f64 probed = 0.0;
        line.clear();
        for (const Entry& entry : entries) {
            probed += entry.ms;
            if (entry.ms < 0.5) {
                continue;
            }
            line += ' ';
            line += entry.name;
            char buffer[24];
            std::snprintf(buffer, sizeof(buffer), "=%.1f", entry.ms);
            line += buffer;
        }
        LOG_WARN("frame spike {:.1f} ms (probed {:.1f}):{}", total, probed,
                 line.empty() ? " (all blocks < 0.5 ms)" : line.c_str());
    }

    struct Entry {
        const char* name;
        f64 ms;
    };
    // The current frame's scopes so far — the perf HUD reads this for
    // its CPU column (valid between the scopes and beginFrame; names are
    // static literals).
    const vector<Entry>& currentEntries() const { return entries; }

private:
    vector<Entry> entries;
    std::string line;
    TimePoint frameStart {};
};

} // namespace core
