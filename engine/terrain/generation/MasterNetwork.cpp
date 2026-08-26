#include "engine/terrain/generation/MasterNetwork.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <glm/glm.hpp>

#include "engine/terrain/generation/FluvialErosion.hpp"

namespace render::terraingen {

namespace {

// Process-level memo of computeMasterNetwork (the B5 plan's promised
// memoization): the routing is a pure function, and since the fleuve
// imprint every stage-1 bake asks for the SAME super cells, recomputing
// them per window multiplied the bake cost (measured +17 s/tile — 9
// stage-1 windows x ~4 cells x ~0.5 s). A hit requires EXACT parameter
// equality (memcmp / field compare) — equal inputs make the cached pure
// result correct by construction; padding-driven false misses only cost
// a recompute. Thread-safe: bake workers share it.
bool sameTrivial(const void* a, const void* b, size_t n) {
    return std::memcmp(a, b, n) == 0;
}

bool sameMacro(const MacroParams& a, const MacroParams& b) {
    if (a.tiers.size() != b.tiers.size()) {
        return false;
    }
    for (size_t i = 0; i < a.tiers.size(); ++i) {
        if (!sameTrivial(&a.tiers[i], &b.tiers[i], sizeof(TierLevel))) {
            return false;
        }
    }
    return a.seaLevel == b.seaLevel && a.seaFloor == b.seaFloor &&
           a.shallowDepth == b.shallowDepth &&
           a.shelfWidth == b.shelfWidth && a.shelfDepth == b.shelfDepth &&
           a.shelfEnd == b.shelfEnd && a.seaFalloff == b.seaFalloff &&
           a.shoreWidth == b.shoreWidth &&
           a.shoreHeight == b.shoreHeight &&
           a.cliffTierStart == b.cliffTierStart &&
           a.cliffTierEnd == b.cliffTierEnd &&
           a.hillChainWavelength == b.hillChainWavelength &&
           a.valleyStretch == b.valleyStretch &&
           a.terraceStep == b.terraceStep &&
           a.terraceEdge == b.terraceEdge &&
           a.warpWavelength == b.warpWavelength &&
           a.warpStrength == b.warpStrength &&
           a.recurveLow == b.recurveLow && a.recurveMid == b.recurveMid &&
           a.recurveHigh == b.recurveHigh &&
           a.recurveSpan == b.recurveSpan;
}

struct NetworkMemo {
    ProceduralControlParams controls;
    MacroParams macro;
    MasterNetworkParams params;
    MasterNetwork net;
};
std::mutex gNetworkMemoMutex;
std::unordered_map<u64, vector<sptr<const NetworkMemo>>> gNetworkMemo;

MasterNetwork computeMasterNetworkUncached(
    const ProceduralControls& controls, const MacroParams& macro,
    const MasterNetworkParams& params, i32 superX, i32 superZ);

sptr<const NetworkMemo> masterNetworkFor(const ProceduralControls& controls,
                                         const MacroParams& macro,
                                         const MasterNetworkParams& params,
                                         i32 superX, i32 superZ) {
    const u64 key = (static_cast<u64>(controls.params().seed) << 32) ^
                    (static_cast<u64>(static_cast<u32>(superX)) << 16) ^
                    static_cast<u64>(static_cast<u32>(superZ));
    {
        std::lock_guard<std::mutex> lock { gNetworkMemoMutex };
        const auto it = gNetworkMemo.find(key);
        if (it != gNetworkMemo.end()) {
            for (const sptr<const NetworkMemo>& memo : it->second) {
                if (sameTrivial(&memo->controls, &controls.params(),
                                sizeof(ProceduralControlParams)) &&
                    sameTrivial(&memo->params, &params,
                                sizeof(MasterNetworkParams)) &&
                    sameMacro(memo->macro, macro)) {
                    return memo;
                }
            }
        }
    }
    auto memo = std::make_shared<NetworkMemo>();
    memo->controls = controls.params();
    memo->macro = macro;
    memo->params = params;
    memo->net = computeMasterNetworkUncached(controls, macro, params,
                                             superX, superZ);
    {
        std::lock_guard<std::mutex> lock { gNetworkMemoMutex };
        gNetworkMemo[key].push_back(memo);
    }
    return memo;
}

} // namespace

MasterNetwork computeMasterNetwork(const ProceduralControls& controls,
                                   const MacroParams& macro,
                                   const MasterNetworkParams& params,
                                   i32 superX, i32 superZ) {
    return masterNetworkFor(controls, macro, params, superX, superZ)->net;
}

namespace {

MasterNetwork computeMasterNetworkUncached(
    const ProceduralControls& controls, const MacroParams& macro,
    const MasterNetworkParams& params, i32 superX, i32 superZ) {
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
        // HALF-OPEN core: a head exactly on the boundary must belong
        // to ONE super cell, or two owners trace it through different
        // windows and publish two truncations of the same river.
        if (hx < coreMinX || hx >= coreMinX + coreMax ||
            hz < coreMinZ || hz >= coreMinZ + coreMax) {
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

} // namespace

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
            // Through the memo: the shared network is read in place,
            // only the touching courses are copied out.
            const auto memo =
                masterNetworkFor(controls, macro, params, sx, sz);
            for (const MasterRiver& river : memo->net.rivers) {
                bool touches = false;
                for (const MasterNode& node : river.nodes) {
                    if (node.x >= minX && node.x <= maxX &&
                        node.z >= minZ && node.z <= maxZ) {
                        touches = true;
                        break;
                    }
                }
                if (touches) {
                    out.push_back(river);
                }
            }
        }
    }
    return out;
}

void imprintMasterChannels(const GridSpec& sim, MacroResult& macro,
                           vector<f32>& extraKeep,
                           const ProceduralControls& controls,
                           const MacroParams& macroParams,
                           const MasterNetworkParams& network,
                           const MasterImprintParams& imprint,
                           f32 widthCoef, f32 widthExponent,
                           f32 fleuveWidthScale) {
    const f32 simMaxX =
        sim.originX + static_cast<f32>(sim.n - 1) * sim.texelSize;
    const f32 simMaxZ =
        sim.originZ + static_cast<f32>(sim.n - 1) * sim.texelSize;
    const auto rivers =
        masterRiversNear(controls, macroParams, network, sim.originX,
                         sim.originZ, simMaxX, simMaxZ);
    const i32 n = static_cast<i32>(sim.n);
    vector<f32> surf;
    for (const MasterRiver& river : rivers) {
        if (river.nodes.size() < 2) {
            continue;
        }
        // Reprofile: downstream lower-only walk with a minimum
        // gradient — the flood-flat stretches of the routed surface
        // become a bed that FLOWS instead of ponding.
        surf.resize(river.nodes.size());
        surf[0] = river.nodes[0].surface;
        for (size_t k = 1; k < river.nodes.size(); ++k) {
            const f32 dist = std::hypot(
                river.nodes[k].x - river.nodes[k - 1].x,
                river.nodes[k].z - river.nodes[k - 1].z);
            surf[k] = glm::max(
                glm::min(surf[k - 1] - imprint.minGradient * dist,
                         river.nodes[k].surface),
                network.seaLevel - 2.0f);
        }
        for (size_t s = 0; s + 1 < river.nodes.size(); ++s) {
            const MasterNode& a = river.nodes[s];
            const MasterNode& b = river.nodes[s + 1];
            // Width law shared with classifyRivers: the imprinted
            // channel and the promoted ribbon agree by construction.
            const auto halfOf = [&](f32 area) {
                return glm::clamp(widthCoef *
                                      std::pow(glm::max(area, 0.0f),
                                               widthExponent),
                                  12.0f, 45.0f) *
                       fleuveWidthScale;
            };
            const f32 halfA = halfOf(a.area);
            const f32 halfB = halfOf(b.area);
            const f32 reach =
                glm::max(halfA, halfB) * imprint.plainFactor +
                sim.texelSize;
            const i32 c0 = static_cast<i32>(
                std::floor((glm::min(a.x, b.x) - reach - sim.originX) /
                           sim.texelSize));
            const i32 c1 = static_cast<i32>(
                std::ceil((glm::max(a.x, b.x) + reach - sim.originX) /
                          sim.texelSize));
            const i32 r0 = static_cast<i32>(
                std::floor((glm::min(a.z, b.z) - reach - sim.originZ) /
                           sim.texelSize));
            const i32 r1 = static_cast<i32>(
                std::ceil((glm::max(a.z, b.z) + reach - sim.originZ) /
                          sim.texelSize));
            const f32 abx = b.x - a.x;
            const f32 abz = b.z - a.z;
            const f32 abLen2 = abx * abx + abz * abz;
            for (i32 row = glm::max(r0, 0);
                 row <= glm::min(r1, n - 1); ++row) {
                for (i32 col = glm::max(c0, 0);
                     col <= glm::min(c1, n - 1); ++col) {
                    const f32 x = sim.x(static_cast<u32>(col));
                    const f32 z = sim.z(static_cast<u32>(row));
                    const f32 t =
                        abLen2 > 0.0f
                            ? glm::clamp(((x - a.x) * abx +
                                          (z - a.z) * abz) /
                                             abLen2,
                                         0.0f, 1.0f)
                            : 0.0f;
                    const f32 px = a.x + abx * t;
                    const f32 pz = a.z + abz * t;
                    const f32 d = std::hypot(x - px, z - pz);
                    const f32 halfT = glm::mix(halfA, halfB, t);
                    const f32 plainT = halfT * imprint.plainFactor;
                    if (d >= plainT) {
                        continue;
                    }
                    const f32 surfT =
                        glm::mix(surf[s], surf[s + 1], t);
                    const size_t i =
                        static_cast<size_t>(row) * sim.n +
                        static_cast<size_t>(col);
                    f32 keepT;
                    if (d < halfT) {
                        // The channel: parabolic bed under the
                        // reprofiled surface. min() only — never a dam;
                        // a locally deeper valley keeps its floor.
                        const f32 nd = d / glm::max(halfT, 0.01f);
                        macro.height[i] = glm::min(
                            macro.height[i],
                            surfT -
                                imprint.bedDepth * (1.0f - nd * nd));
                        keepT = imprint.keepChannel;
                    } else {
                        // The alluvial plain: a bounded flatten toward
                        // the waterline — deeper crossings stay GORGES
                        // (only the channel cuts them).
                        const f32 f =
                            (d - halfT) / glm::max(plainT - halfT,
                                                   0.01f);
                        const f32 cap =
                            surfT + imprint.plainLift + f * 6.0f;
                        macro.height[i] = glm::min(
                            macro.height[i],
                            glm::max(cap, macro.height[i] -
                                              imprint.maxPlainCut));
                        keepT = imprint.keepPlain * (1.0f - f);
                    }
                    extraKeep[i] = glm::max(extraKeep[i], keepT);
                    // The course claims its floor for the habitable/
                    // soft-erosion families (future site scoring reads
                    // the same fields).
                    const f32 band =
                        1.0f - glm::clamp((d - halfT) /
                                              glm::max(plainT - halfT,
                                                       0.01f),
                                          0.0f, 1.0f);
                    macro.calm[i] =
                        glm::max(macro.calm[i], 0.8f * band);
                    macro.gentle[i] =
                        glm::max(macro.gentle[i], 0.6f * band);
                }
            }
        }
    }
}

} // namespace render::terraingen
