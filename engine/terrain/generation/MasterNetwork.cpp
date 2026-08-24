#include "engine/terrain/generation/MasterNetwork.hpp"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

#include "engine/terrain/generation/FluvialErosion.hpp"

namespace render::terraingen {

MasterNetwork computeMasterNetwork(const ProceduralControls& controls,
                                   const MacroParams& macro,
                                   const MasterNetworkParams& params,
                                   i32 superX, i32 superZ) {
    MasterNetwork out;
    const f32 originX =
        static_cast<f32>(superX) * params.superRegionSize - params.apron;
    const f32 originZ =
        static_cast<f32>(superZ) * params.superRegionSize - params.apron;
    const f32 span = params.superRegionSize + 2.0f * params.apron;
    const u32 n = static_cast<u32>(std::lround(span / params.texel)) + 1;
    out.grid = GridSpec { originX, originZ, params.texel, n };

    vector<f32> height(out.grid.cells());
    for (u32 row = 0; row < n; ++row) {
        for (u32 col = 0; col < n; ++col) {
            height[static_cast<size_t>(row) * n + col] =
                macroHeightAnalytic(controls, macro, out.grid.x(col),
                                    out.grid.z(row));
        }
    }
    const vector<f32> filled = priorityFloodFill(
        out.grid, height, params.seaLevel, params.minSlope);
    const FlowRouting flow =
        routeFlow(out.grid, filled, height, params.seaLevel);

    // Channel cells: TRUE drainage above the fleuve threshold, on dry
    // ground. Heads = channel cells fed by no channel donor.
    const size_t cells = out.grid.cells();
    vector<u8> channel(cells, 0);
    vector<u8> hasChannelDonor(cells, 0);
    for (size_t i = 0; i < cells; ++i) {
        channel[i] = flow.area[i] >= params.fleuveArea &&
                     height[i] > params.seaLevel;
    }
    for (size_t i = 0; i < cells; ++i) {
        if (channel[i] && flow.receiver[i] != i) {
            hasChannelDonor[flow.receiver[i]] |= channel[i];
        }
    }

    // Ownership: a river belongs to the super cell holding its head —
    // every caller computing this cell gets the same courses.
    const f32 coreMinX = originX + params.apron;
    const f32 coreMinZ = originZ + params.apron;
    const f32 coreMax = params.superRegionSize;
    vector<u8> claimed(cells, 0);
    // Deterministic head order: grid order (row-major), no float sort.
    for (size_t head = 0; head < cells; ++head) {
        if (!channel[head] || hasChannelDonor[head]) {
            continue;
        }
        const f32 hx =
            out.grid.x(static_cast<u32>(head % n));
        const f32 hz =
            out.grid.z(static_cast<u32>(head / n));
        if (hx < coreMinX || hx > coreMinX + coreMax ||
            hz < coreMinZ || hz > coreMinZ + coreMax) {
            continue; // apron head: the neighbour cell owns it
        }
        MasterRiver river;
        size_t i = head;
        while (true) {
            const u32 col = static_cast<u32>(i % n);
            const u32 row = static_cast<u32>(i / n);
            river.nodes.push_back({ out.grid.x(col), out.grid.z(row),
                                    filled[i], flow.area[i] });
            if (claimed[i]) {
                break; // joined an earlier course (confluence)
            }
            claimed[i] = 1;
            const u32 r = flow.receiver[i];
            if (r == i) {
                river.reachesSea = height[i] <= params.seaLevel + 0.5f;
                break; // base level: sea or window rim
            }
            if (height[r] <= params.seaLevel) {
                river.reachesSea = true;
                break;
            }
            i = r;
        }
        if (river.nodes.size() >= 4) {
            out.rivers.push_back(std::move(river));
        }
    }
    return out;
}

vector<MasterRiver> masterRiversNear(const ProceduralControls& controls,
                                     const MacroParams& macro,
                                     const MasterNetworkParams& params,
                                     f32 minX, f32 minZ, f32 maxX,
                                     f32 maxZ) {
    vector<MasterRiver> out;
    const i32 sx0 = static_cast<i32>(
        std::floor(minX / params.superRegionSize));
    const i32 sx1 = static_cast<i32>(
        std::floor(maxX / params.superRegionSize));
    const i32 sz0 = static_cast<i32>(
        std::floor(minZ / params.superRegionSize));
    const i32 sz1 = static_cast<i32>(
        std::floor(maxZ / params.superRegionSize));
    for (i32 sz = sz0; sz <= sz1; ++sz) {
        for (i32 sx = sx0; sx <= sx1; ++sx) {
            MasterNetwork network =
                computeMasterNetwork(controls, macro, params, sx, sz);
            for (MasterRiver& river : network.rivers) {
                bool touches = false;
                for (const MasterNode& node : river.nodes) {
                    if (node.x >= minX && node.x <= maxX &&
                        node.z >= minZ && node.z <= maxZ) {
                        touches = true;
                        break;
                    }
                }
                if (touches) {
                    out.push_back(std::move(river));
                }
            }
        }
    }
    return out;
}

} // namespace render::terraingen
