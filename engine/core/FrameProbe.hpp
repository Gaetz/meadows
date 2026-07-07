#pragma once

#include <chrono>
#include <string>

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
    using Clock = std::chrono::steady_clock;

    static f64 msSince(Clock::time_point start) {
        return std::chrono::duration<f64, std::milli>(Clock::now() - start)
            .count();
    }

public:
    void beginFrame() {
        entries.clear();
        frameStart = Clock::now();
    }

    class Scope {
    public:
        Scope(FrameProbe& probe, const char* name)
            : probe { probe }, name { name }, start { Clock::now() } {}
        ~Scope() { probe.entries.push_back({ name, msSince(start) }); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        FrameProbe& probe;
        const char* name;
        Clock::time_point start;
    };

    // Logs the breakdown when the frame exceeded `thresholdMs`. Blocks
    // under 0.5 ms are elided (noise); `probed` = their sum, so a gap
    // between it and the total points outside the instrumented blocks.
    void endFrame(f64 thresholdMs = 25.0) {
        const f64 total = msSince(frameStart);
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

private:
    struct Entry {
        const char* name;
        f64 ms;
    };
    vector<Entry> entries;
    std::string line;
    Clock::time_point frameStart {};
};

} // namespace core
