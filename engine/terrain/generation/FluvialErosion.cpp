#include "engine/terrain/generation/FluvialErosion.hpp"

#include <algorithm>
#include <cmath>
#include <queue>

#include <glm/glm.hpp>

namespace render::terraingen {

namespace {

struct Neighbour {
    i32 dx;
    i32 dz;
    f32 dist; // texel units, scaled by texelSize at use
};

constexpr Neighbour kNeighbours[8] = {
    { -1, 0, 1.0f },          { 1, 0, 1.0f },
    { 0, -1, 1.0f },          { 0, 1, 1.0f },
    { -1, -1, 1.41421356f },  { 1, -1, 1.41421356f },
    { -1, 1, 1.41421356f },   { 1, 1, 1.41421356f },
};

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
        for (const Neighbour& nb : kNeighbours) {
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
            for (const Neighbour& nb : kNeighbours) {
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
                           const vector<f32>* erodibility) {
    const size_t cells = spec.cells();
    FluvialResult out;
    out.height = height;

    FlowRouting flow;
    const i32 interval = glm::max(params.routingInterval, 1);
    for (i32 iter = 0; iter < params.iterations; ++iter) {
        if (iter % interval == 0) {
            // Depression-routed surface: every node can reach base
            // level. Reused for `interval` iterations (see the param).
            const vector<f32> filled = priorityFloodFill(
                spec, out.height, params.seaLevel, params.minSlope);
            flow = routeFlow(spec, filled, out.height, params.seaLevel);
        }
        // Implicit update, ascending: the receiver's NEW height is
        // already known when its donors solve — one O(n) pass down the
        // drainage tree, no CFL limit (Braun & Willett 2013).
        for (size_t k = 0; k < cells; ++k) {
            const u32 i = flow.order[k];
            const u32 r = flow.receiver[i];
            if (r == i) {
                continue; // base level: fixed
            }
            const f32 ks = erodibility ? (*erodibility)[i] : 1.0f;
            const f32 f = params.dt * params.k * ks *
                          std::pow(flow.area[i], params.areaExponent) /
                          flow.recvDist[i];
            const f32 raised =
                out.height[i] + params.dt * params.upliftRate * uplift[i];
            out.height[i] = (raised + f * out.height[r]) / (1.0f + f);
        }
        out.area = flow.area;
    }

    if (keep) {
        for (size_t i = 0; i < cells; ++i) {
            out.height[i] =
                glm::mix(out.height[i], height[i], (*keep)[i]);
        }
    }
    return out;
}

} // namespace render::terraingen
