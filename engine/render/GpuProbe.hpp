#pragma once

#include <unordered_map>

#include "engine/core/Defines.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

// The GPU half of core::FrameProbe: per-pass GPU
// milliseconds without ever stalling. Wrap each pass's command
// RECORDING in a Scope; a GL timestamp records when the GPU REACHES
// that point of the stream — on a single queue the delta between a
// scope's two timestamps IS the pass's GPU time.
//
// Discipline (the lesson the fences taught): results are read
// FRAMES LATER. A ring of kFramesInFlight slots holds the pending
// timestamps; beginFrame() polls only the OLDEST slot and resolves it
// only when every one of its timestamps is available — never blocks.
// If the ring is full and the oldest is still pending, the current
// frame simply doesn't instrument (back-pressure). Everything no-ops on
// devices without caps().timerQueries.
class GpuProbe {
public:
    class Scope {
    public:
        Scope(GpuProbe& probe, rhi::Device& device, const char* name)
            : probe { &probe }, device { &device }, name { name } {
            begin = probe.mark(device);
        }
        // Null-tolerant overload: deep call sites (PostFx) take an
        // optional probe and instrument only when one is passed.
        Scope(GpuProbe* probe, rhi::Device* device, const char* name)
            : probe { probe }, device { device }, name { name } {
            if (probe && device) {
                begin = probe->mark(*device);
            }
        }
        ~Scope() {
            if (probe && device) {
                probe->record(*device, name, begin);
            }
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        GpuProbe* probe;
        rhi::Device* device;
        const char* name;
        rhi::TimestampHandle begin {};
    };

    // Resolves the oldest slot when ready, then opens the next frame's
    // slot (or skips instrumentation when the ring is saturated).
    void beginFrame(rhi::Device& device);

    // Rolling stats over the resolve window (~120 resolved frames).
    struct PassStats {
        f64 averageMs { 0.0 };
        f64 maxMs { 0.0 };
    };
    // Ordered like the frame's scopes; empty until the first resolve.
    struct PassRow {
        const char* name { nullptr };
        PassStats stats;
    };
    const vector<PassRow>& rows() const { return rowsCache; }
    f64 frameAverageMs() const { return frameStats.averageMs; }
    f64 frameMaxMs() const { return frameStats.maxMs; }
    bool active() const { return enabled; }
    void resetWindow();

    void shutdown(rhi::Device& device); // teardown: abandon in-flight

private:
    static constexpr u32 kFramesInFlight = 4;
    static constexpr u32 kWindow = 120; // resolved frames per stats window

    struct Sample {
        const char* name;
        rhi::TimestampHandle begin;
        rhi::TimestampHandle end;
    };
    struct Slot {
        vector<Sample> samples;
        u64 frameIndex { 0 };
        bool open { false };
    };

    rhi::TimestampHandle mark(rhi::Device& device);
    void record(rhi::Device& device, const char* name,
                rhi::TimestampHandle begin);
    void resolveOldest(rhi::Device& device);
    void accumulate(const char* name, f64 ms);

    Slot slots[kFramesInFlight];
    u32 head { 0 }; // slot being recorded this frame
    u32 tail { 0 }; // oldest unresolved slot
    u32 pending { 0 };
    bool recording { false };
    bool enabled { true };
    u64 frameIndex { 0 };

    struct Accum {
        f64 sum { 0.0 };
        f64 max { 0.0 };
        u32 count { 0 };
    };
    std::unordered_map<const char*, Accum> accums;
    Accum frameAccum;
    u32 resolvedInWindow { 0 };
    PassStats frameStats;
    vector<PassRow> rowsCache;
    vector<const char*> rowOrder;
};

} // namespace render
