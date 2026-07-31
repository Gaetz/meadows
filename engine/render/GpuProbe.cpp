#include "engine/render/GpuProbe.hpp"

#include <algorithm>

#include "engine/core/Log.hpp"

namespace render {

rhi::TimestampHandle GpuProbe::mark(rhi::Device& device) {
    if (!recording) {
        return {};
    }
    return device.insertTimestamp();
}

void GpuProbe::record(rhi::Device& device, const char* name,
                      rhi::TimestampHandle begin) {
    if (!recording || begin.id == 0) {
        return;
    }
    const rhi::TimestampHandle end = device.insertTimestamp();
    if (end.id == 0) {
        device.destroyTimestamp(begin);
        return;
    }
    slots[head].samples.push_back({ name, begin, end });
}

void GpuProbe::beginFrame(rhi::Device& device) {
    enabled = device.caps().timerQueries;
    if (!enabled) {
        recording = false;
        return;
    }
    // Close the slot recorded LAST frame.
    if (recording) {
        slots[head].open = true;
        head = (head + 1) % kFramesInFlight;
        ++pending;
        recording = false;
    }
    // Resolve the oldest closed slot when every timestamp is available.
    if (pending > 0) {
        resolveOldest(device);
    }
    // Open this frame's slot unless the ring is saturated (back-pressure:
    // skip instrumentation rather than stall — the fence lesson).
    if (pending < kFramesInFlight) {
        slots[head].samples.clear();
        slots[head].frameIndex = ++frameIndex;
        slots[head].open = false;
        recording = true;
    }
}

void GpuProbe::resolveOldest(rhi::Device& device) {
    Slot& slot = slots[tail];
    if (!slot.open) {
        return;
    }
    // Cross-queue safety: a slot can hold timestamps from BOTH the graphics
    // and the async-compute stream (the rc* scopes). The last-sample gate
    // below only proves the GRAPHICS stream retired — that frame's compute
    // submission starts after the graphics submit and can still be in
    // flight, and a not-ready compute sample would read as garbage. Two
    // device frames later the backend's beginFrame has provably waited both
    // (slot fence + compute timeline), so hold resolution until then.
    if (frameIndex < slot.frameIndex + 2) {
        return;
    }
    // All-or-nothing: GL returns results in submission order, so if the
    // LAST timestamp is ready the whole slot is — but poll each anyway
    // (drivers may differ) without consuming until all are available...
    // consuming as we go is fine BECAUSE we only commit stats when the
    // full slot resolved this frame; a partially-consumed slot just
    // finishes next frame (values are cached in the sample).
    // Simpler contract: check readiness back to front; the last sample's
    // end timestamp gates the slot.
    u64 nanos = 0;
    if (slot.samples.empty()) {
        slot.open = false;
        tail = (tail + 1) % kFramesInFlight;
        --pending;
        return;
    }
    // Gate: is the final timestamp available? (order guarantees the rest)
    const rhi::TimestampHandle last = slot.samples.back().end;
    if (!device.timestampReady(last, nanos)) {
        return; // still in flight — try again next frame
    }
    const u64 frameEnd = nanos;
    u64 frameBegin = 0;
    f64 spikeCheck = 0.0;
    str spikeLine;
    for (size_t i = 0; i < slot.samples.size(); ++i) {
        Sample& sample = slot.samples[i];
        u64 beginNs = 0;
        u64 endNs = frameEnd;
        device.timestampReady(sample.begin, beginNs); // ready by order
        if (i + 1 < slot.samples.size() || sample.end.id != last.id) {
            device.timestampReady(sample.end, endNs);
        }
        if (i == 0) {
            frameBegin = beginNs;
        }
        frameBegin = frameBegin == 0 ? beginNs : std::min(frameBegin, beginNs);
        const f64 ms =
            static_cast<f64>(endNs - std::min(beginNs, endNs)) / 1.0e6;
        accumulate(sample.name, ms);
        if (ms >= 0.5) {
            char buffer[48];
            std::snprintf(buffer, sizeof(buffer), " %s=%.1f", sample.name,
                          ms);
            spikeLine += buffer;
        }
        spikeCheck = std::max(spikeCheck, ms);
    }
    const f64 totalMs = static_cast<f64>(frameEnd - frameBegin) / 1.0e6;
    frameAccum.sum += totalMs;
    frameAccum.max = std::max(frameAccum.max, totalMs);
    ++frameAccum.count;
    if (totalMs > 25.0) {
        // Throttled to one line every ~5 s: on a machine where EVERY frame
        // exceeds the budget (M1 Debug through MoltenVK), per-frame spike
        // lines drown the log without adding information — the perf panel
        // carries the live numbers.
        static u64 lastSpikeLogFrame = 0;
        if (slot.frameIndex > lastSpikeLogFrame + 300 ||
            lastSpikeLogFrame == 0) {
            lastSpikeLogFrame = slot.frameIndex;
            // The CPU FrameProbe logged its half 2-4 frames earlier; the
            // frame index pairs them.
            LOG_WARN("gpu frame spike {:.1f} ms (frame {}):{}", totalMs,
                     slot.frameIndex,
                     spikeLine.empty() ? " (all passes < 0.5 ms)"
                                       : spikeLine.c_str());
        }
    }

    slot.open = false;
    slot.samples.clear();
    tail = (tail + 1) % kFramesInFlight;
    --pending;

    // Fold the window into the public rows.
    if (++resolvedInWindow >= kWindow) {
        resetWindow();
    } else {
        rowsCache.clear();
        for (const char* name : rowOrder) {
            const Accum& accum = accums[name];
            rowsCache.push_back(
                { name,
                  { accum.count > 0 ? accum.sum / accum.count : 0.0,
                    accum.max } });
        }
        frameStats = { frameAccum.count > 0
                           ? frameAccum.sum / frameAccum.count
                           : 0.0,
                       frameAccum.max };
    }
}

void GpuProbe::accumulate(const char* name, f64 ms) {
    auto [it, inserted] = accums.try_emplace(name);
    if (inserted) {
        rowOrder.push_back(name);
    }
    it->second.sum += ms;
    it->second.max = std::max(it->second.max, ms);
    ++it->second.count;
}

void GpuProbe::resetWindow() {
    accums.clear();
    rowOrder.clear();
    frameAccum = {};
    resolvedInWindow = 0;
    // rowsCache/frameStats keep their last values until fresh data lands
    // (the HUD never blanks).
}

void GpuProbe::shutdown(rhi::Device& device) {
    for (Slot& slot : slots) {
        for (Sample& sample : slot.samples) {
            device.destroyTimestamp(sample.begin);
            device.destroyTimestamp(sample.end);
        }
        slot.samples.clear();
        slot.open = false;
    }
    pending = 0;
    recording = false;
    head = tail = 0;
}

} // namespace render
