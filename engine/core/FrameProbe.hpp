#pragma once

#include <string>

#include "engine/core/Clock.hpp"
#include "engine/core/Defines.hpp"

namespace core {

// Per-frame CPU block breakdown (dev tool). Wrap the frame's suspect
// blocks in Scopes; the perf HUD reads currentEntries() for its CPU
// column. Overhead: a dozen steady_clock reads per frame, no allocation
// after warm-up.
class FrameProbe {
public:
    void beginFrame() { entries.clear(); }

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
};

} // namespace core
