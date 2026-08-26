#include "engine/terrain/generation/FluvialErosion.hpp"
#include "engine/terrain/generation/GridOps.hpp"

#include <algorithm>
#include <cmath>
#include <queue>

#include <glm/glm.hpp>

namespace render::terraingen {

namespace {

bool isBaseLevel(const GridSpec& spec, const vector<f32>& height, u32 col,
                 u32 row, f32 seaLevel) {
    if (col == 0 || row == 0 || col == spec.n - 1 || row == spec.n - 1) {
        return true;
    }
    return height[static_cast<size_t>(row) * spec.n + col] <= seaLevel;
}

} // namespace

vector<f32> priorityFloodFill(const GridSpec& spec,
                              const vector<f32>& height, f32 seaLevel,
                              f32 minSlope) {
    const i32 n = static_cast<i32>(spec.n);
    const f32 eps = minSlope * spec.texelSize;
    vector<f32> filled(height.size());
    vector<u8> visited(height.size(), 0);
    using Entry = std::pair<f32, u32>; // (filled height, index)
    std::priority_queue<Entry, vector<Entry>, std::greater<Entry>> open;
    for (u32 row = 0; row < spec.n; ++row) {
        for (u32 col = 0; col < spec.n; ++col) {
            const size_t i = static_cast<size_t>(row) * spec.n + col;
            if (isBaseLevel(spec, height, col, row, seaLevel)) {
                filled[i] = height[i];
                visited[i] = 1;
                open.emplace(filled[i], static_cast<u32>(i));
            }
        }
    }
    while (!open.empty()) {
        const auto [level, iu] = open.top();
        open.pop();
        const i32 cx = static_cast<i32>(iu % spec.n);
        const i32 cz = static_cast<i32>(iu / spec.n);
        for (const Neighbour& nb : kNeighbours8) {
            const i32 px = cx + nb.dx;
            const i32 pz = cz + nb.dz;
            if (px < 0 || pz < 0 || px >= n || pz >= n) {
                continue;
            }
            const size_t p =
                static_cast<size_t>(pz) * spec.n + static_cast<size_t>(px);
            if (visited[p]) {
                continue;
            }
            visited[p] = 1;
            filled[p] = glm::max(height[p], level + eps);
            open.emplace(filled[p], static_cast<u32>(p));
        }
    }
    return filled;
}

FlowRouting routeFlow(const GridSpec& spec, const vector<f32>& routed,
                      const vector<f32>& trueHeight, f32 seaLevel) {
    const i32 n = static_cast<i32>(spec.n);
    const size_t cells = spec.cells();
    const f32 cellArea = spec.texelSize * spec.texelSize;
    FlowRouting flow;
    flow.receiver.resize(cells);
    flow.recvDist.resize(cells);
    flow.order.resize(cells);
    flow.area.assign(cells, cellArea);
    // Steepest-descent receivers on the routed surface.
    for (i32 cz = 0; cz < n; ++cz) {
        for (i32 cx = 0; cx < n; ++cx) {
            const size_t i = static_cast<size_t>(cz) * spec.n + cx;
            flow.receiver[i] = static_cast<u32>(i);
            flow.recvDist[i] = spec.texelSize;
            if (isBaseLevel(spec, trueHeight, static_cast<u32>(cx),
                            static_cast<u32>(cz), seaLevel)) {
                continue;
            }
            f32 best = 0.0f;
            for (const Neighbour& nb : kNeighbours8) {
                const i32 px = cx + nb.dx;
                const i32 pz = cz + nb.dz;
                if (px < 0 || pz < 0 || px >= n || pz >= n) {
                    continue;
                }
                const size_t p = static_cast<size_t>(pz) * spec.n +
                                 static_cast<size_t>(px);
                const f32 dist = nb.dist * spec.texelSize;
                const f32 slope = (routed[i] - routed[p]) / dist;
                if (slope > best) {
                    best = slope;
                    flow.receiver[i] = static_cast<u32>(p);
                    flow.recvDist[i] = dist;
                }
            }
        }
    }
    // Process order: routed surface ascending (receivers first — strict,
    // since a receiver is always strictly lower on the routed surface).
    for (size_t i = 0; i < cells; ++i) {
        flow.order[i] = static_cast<u32>(i);
    }
    std::sort(flow.order.begin(), flow.order.end(), [&](u32 a, u32 b) {
        return routed[a] < routed[b];
    });
    // Drainage area: descending sweep, donors into receivers.
    for (size_t k = cells; k-- > 0;) {
        const u32 i = flow.order[k];
        if (flow.receiver[i] != i) {
            flow.area[flow.receiver[i]] += flow.area[i];
        }
    }
    return flow;
}

FluvialResult erodeFluvial(const GridSpec& spec, const vector<f32>& height,
                           const vector<f32>& uplift,
                           const FluvialParams& params,
                           const vector<f32>* keep,
                           const vector<f32>* erodibility,
                           const vector<f32>* capacityScale) {
    const size_t cells = spec.cells();
    const f32 cellArea = spec.texelSize * spec.texelSize;
    FluvialResult out;
    out.height = height;

    const bool transport = params.sedimentCapacity > 0.0f;
    vector<f32> eroded;   // m removed this iteration (sediment source)
    vector<f32> sediment; // m³ of flux arriving per cell this iteration
    if (transport) {
        out.deposit.assign(cells, 0.0f);
        eroded.resize(cells);
        sediment.resize(cells);
    }

    FlowRouting flow;
    vector<f32> routed;
    const i32 interval = glm::max(params.routingInterval, 1);
    for (i32 iter = 0; iter < params.iterations; ++iter) {
        if (iter % interval == 0) {
            // Depression-routed surface: every node can reach base
            // level. Reused for `interval` iterations (see the param).
            routed = priorityFloodFill(spec, out.height, params.seaLevel,
                                       params.minSlope);
            flow = routeFlow(spec, routed, out.height, params.seaLevel);
        }
        // Implicit update, ascending: the receiver's NEW height is
        // already known when its donors solve — one O(n) pass down the
        // drainage tree, no CFL limit (Braun & Willett 2013).
        for (size_t k = 0; k < cells; ++k) {
            const u32 i = flow.order[k];
            const u32 r = flow.receiver[i];
            if (r == i) {
                if (transport) {
                    eroded[i] = 0.0f;
                }
                continue; // base level: fixed
            }
            const f32 ks = erodibility ? (*erodibility)[i] : 1.0f;
            const f32 f = params.dt * params.k * ks *
                          std::pow(flow.area[i], params.areaExponent) /
                          flow.recvDist[i];
            const f32 raised =
                out.height[i] + params.dt * params.upliftRate * uplift[i];
            const f32 solved = (raised + f * out.height[r]) / (1.0f + f);
            if (transport) {
                eroded[i] = glm::max(raised - solved, 0.0f);
            }
            out.height[i] = solved;
        }
        // Sediment sweep, descending (donors before receivers, fixed
        // order — deterministic): flux rides the drainage tree and
        // deposits wherever it exceeds the carrying capacity. Depositing
        // never raises a cell above the routed surface (+slack), so the
        // routing stays drained and lake floors cap below their spill.
        if (transport) {
            std::fill(sediment.begin(), sediment.end(), 0.0f);
            for (size_t k = cells; k-- > 0;) {
                const u32 i = flow.order[k];
                const u32 r = flow.receiver[i];
                f32 flux = sediment[i] + eroded[i] * cellArea;
                if (r == i) {
                    continue; // base level: flux exits (sea/rim)
                }
                const f32 slope = glm::max(
                    (routed[i] - routed[r]) / flow.recvDist[i],
                    params.capacitySlopeFloor);
                const f32 cs = capacityScale ? (*capacityScale)[i] : 1.0f;
                const f32 capacity =
                    params.sedimentCapacity * cs *
                    std::pow(flow.area[i], params.areaExponent) * slope *
                    params.dt;
                const f32 excess = flux - capacity;
                if (excess > 0.0f) {
                    // Flooded-aware ceiling: deep lake cells keep
                    // lakeKeepDepth of water under the routed surface —
                    // and CHANNELS keep a drainage-scaled depth of their
                    // own: the old ceiling (routed + slack) let every
                    // river aggrade to its waterline and bury itself in
                    // its own sediment; physically a big river keeps its
                    // discharge and exports (the capacity slope-floor
                    // alone was measured toothless — the ceiling is what
                    // plugged the beds).
                    const f32 waterDepth = routed[i] - out.height[i];
                    const f32 channelKeep =
                        params.channelKeepCoef > 0.0f &&
                                flow.area[i] > params.channelKeepMinArea
                            ? glm::min(params.channelKeepCoef *
                                           std::pow(flow.area[i],
                                                    params
                                                        .channelKeepExponent),
                                       params.channelKeepMax)
                            : 0.0f;
                    const f32 keepDepth = glm::max(
                        waterDepth > params.lakeKeepDepth
                            ? params.lakeKeepDepth
                            : 0.0f,
                        channelKeep);
                    const f32 ceiling =
                        keepDepth > 0.0f
                            ? routed[i] - keepDepth
                            : routed[i] + params.depositSlack;
                    const f32 room =
                        glm::max(ceiling - out.height[i], 0.0f);
                    const f32 d = glm::min(
                        glm::min(excess / cellArea, params.depositMax),
                        room);
                    out.height[i] += d;
                    out.deposit[i] += d;
                    flux -= d * cellArea;
                }
                sediment[r] += flux;
            }
        }
    }
    out.area = std::move(flow.area);

    if (keep) {
        for (size_t i = 0; i < cells; ++i) {
            out.height[i] =
                glm::mix(out.height[i], height[i], (*keep)[i]);
        }
    }
    return out;
}

} // namespace render::terraingen
