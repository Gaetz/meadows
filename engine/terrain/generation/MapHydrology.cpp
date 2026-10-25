#include "engine/terrain/generation/MapHydrology.hpp"

#include <algorithm>
#include <cmath>
#include <queue>

#include <glm/glm.hpp>

#include "engine/terrain/generation/GridOps.hpp"

namespace render::terraingen {

f32 MapGroundSampler::at(f32 x, f32 z) const {
    const f32 s1 = bilinearWorld(mapS1.sim, mapS1.eroded, x, z);
    // Among the covering slices, the one holding the point
    // DEEPEST inside wins: a slice's outer kFineErosionHalo ring
    // is edge-effect territory (sampling it flooded a buried
    // lake 40 m under the owner slice's real ground, measured on
    // a slice line).
    const FinalizeResult* best = nullptr;
    f32 bestMargin = -1.0e9f;
    for (const FinalizeResult* g : groundSlices) {
        if (!g || g->height.empty()) {
            continue;
        }
        const GridSpec& fs = g->fineSpec;
        const f32 maxX =
            fs.originX + static_cast<f32>(fs.n - 1) * fs.texelSize;
        const f32 maxZ =
            fs.originZ + static_cast<f32>(fs.n - 1) * fs.texelSize;
        const f32 margin = glm::min(
            glm::min(x - fs.originX, maxX - x),
            glm::min(z - fs.originZ, maxZ - z));
        if (margin < 0.0f) {
            continue;
        }
        if (margin > bestMargin) {
            bestMargin = margin;
            best = g;
        }
    }
    const f32 inner = best
                          ? bilinearWorld(best->fineSpec,
                                          best->height, x, z)
                          : s1;
    const f32 dOut = glm::max(
        glm::max(mapMinX - x, x - (mapMinX + mapSize)),
        glm::max(mapMinZ - z, z - (mapMinZ + mapSize)));
    if (dOut <= 0.0f) {
        return inner;
    }
    // Past the rect: a cached NEIGHBOUR map's published final
    // ground is truer than our own halo or the stage-1 — the
    // shared margin's basins then resolve on the terrain both
    // maps actually publish (the remaining water seam at lines).
    // Documented impurity: the margin water refines with the bake
    // order; a map baked alone stays bit-exact.
    for (const render::TerrainRegion& nr : neighbourRegions) {
        const f32 fx = (x - nr.originX) / nr.texelSize;
        const f32 fz = (z - nr.originZ) / nr.texelSize;
        if (fx < 0.0f || fz < 0.0f ||
            fx > static_cast<f32>(nr.width) - 1.001f ||
            fz > static_cast<f32>(nr.height) - 1.001f) {
            continue;
        }
        const u32 c = static_cast<u32>(fx);
        const u32 r = static_cast<u32>(fz);
        const f32 tx = fx - static_cast<f32>(c);
        const f32 tz = fz - static_cast<f32>(r);
        const auto nrAt = [&](u32 cc, u32 rr) {
            return nr.heights[static_cast<size_t>(rr) * nr.width +
                              cc];
        };
        const f32 nH = glm::mix(
            glm::mix(nrAt(c, r), nrAt(c + 1, r), tx),
            glm::mix(nrAt(c, r + 1), nrAt(c + 1, r + 1), tx), tz);
        return glm::mix(inner, nH,
                        glm::smoothstep(0.0f, 96.0f, dOut));
    }
    if (!best) {
        return s1;
    }
    const f32 w = 1.0f - glm::smoothstep(0.0f, 192.0f, dOut);
    return glm::mix(s1, inner, w);
}

namespace {

// The water sits JUST UNDER the lowest fine ground of its
// footprint (ribbon + carve shoulder): nothing beyond the
// ribbon edge may stay below the waterline. A 5-point
// longitudinal min covers the texel rows between points (~8 m
// spacing) and absorbs sampling jitter; the pits it would
// have dragged along in a clamp-only pass are bridged by the
// pooling fill instead.
constexpr f32 kEdgeTuck = 0.2f;
// Ceiling of the fill: no point's water may stand more than a
// pond's depth above the LOWEST fine floor upstream of it —
// the no-floating invariant. A course routed along a fine
// ridge the window never saw (its fine floor RISES downstream
// by tens of metres) is tunneled at the upstream valley level,
// buried and invisible under the ridge (the accepted trade;
// the re-route brick owns those courses), instead of pooling
// the whole upstream valley into a floating sheet — a plain
// routing-level ceiling was measured 38 m above the valley
// floor on such a reach. The slow recovery keeps one deep
// mis-sampled pit from dragging its penalty kilometres
// downstream (the lesson of the old through-cut).
constexpr f32 kPoolMax = 2.5f;        // wall-tunnel slack (m)
constexpr f32 kBoundRecover = 0.01f;  // m per m of course

// A course's mouth within confluence reach of another course drains
// INTO it: `receiver` is that course (-1 = free mouth), `recvIdx` the
// receiver point the mouth lands on.
struct JunctionLink {
    i32 receiver { -1 };
    size_t recvIdx { 0 };
};

// The re-leveling state shared by the passes below: the hydrology
// being corrected, the fine ground sampler, the per-course envelopes
// and the junction graph. One member function per pass; the driver
// (relevelMapHydrology) runs them in the fixed order of
// kRelevelPasses.
//
// INVARIANT — the envelopes (floors, bounds, lakeLvls, oldSurfaces)
// and the junction graph (links, order) are built ONCE, from the
// courses and lakes as they stand after the lake re-seat, and are
// FROZEN from then on: the pool discovery adds lakes and lowers
// levels, the later passes move surfaces and half-widths, yet every
// clamp keeps reading the envelopes of that state. Rebuilding them
// mid-chain changes the published profiles.
struct HydroReleveling {
    const TileBakeParams& params;
    const HydrologyParams& hydrology; // params.hydrology + seaLevel
    const MapGroundSampler& ground;   // phase-A ground, pre-pierce
    const vector<f32>& preGround;     // window ground before piercing
    MapHydrology& out;
    vector<River>& rivers; // = out.hydro.rivers
    const size_t riverCount;
    const f32 winMaxX;
    const f32 winMaxZ;

    // Per-course envelopes (see the invariant above).
    vector<vector<f32>> floors;      // 5-point footprint floor
    vector<vector<f32>> bounds;      // slope-limited fill ceiling
    vector<vector<f32>> lakeLvls;    // covering accepted-lake level
    vector<vector<f32>> oldSurfaces; // routed surface, pre-rebuild
    vector<JunctionLink> links;
    vector<size_t> order; // receivers before their tributaries

    HydroRelevelStats stats;

    HydroReleveling(const TileBakeParams& p, const HydrologyParams& h,
                    const MapGroundSampler& g, const vector<f32>& pre,
                    MapHydrology& o)
        : params(p), hydrology(h), ground(g), preGround(pre), out(o),
          rivers(o.hydro.rivers), riverCount(o.hydro.rivers.size()),
          winMaxX(o.window.originX +
                  static_cast<f32>(o.window.n - 1) * o.window.texelSize),
          winMaxZ(o.window.originZ +
                  static_cast<f32>(o.window.n - 1) * o.window.texelSize) {
    }

    // The fine ground the carves will see: the phase-A ground plus
    // the pierce notches (which exist only as a window delta until
    // phase C transfers them, applyPierceDelta — the same carve-only
    // transfer is sampled here). Reads out.pierceDelta LIVE, so the
    // saddle cut's own notches feed the courses cut after them.
    f32 fineAt(f32 x, f32 z) const;

    void recenterCourses();
    void retraceBuriedRuns();
    void reseatWindowLakes();
    void buildEnvelopes();
    void linkTributaries();
    void fillProfiles();
    void discoverPools();
    void coalesceCascades();
    void capSlopes();
    void smoothProfiles();
    void narrowSteep();
    void reclampTributaries();
    void softenMapSeams();
    void reseatPonds();
    void dropRapidsFords();
    void cutSaddles();
};

f32 HydroReleveling::fineAt(f32 x, f32 z) const {
    f32 g = ground.at(x, z);
    if (!out.pierceDelta.empty() && x >= out.window.originX &&
        x <= winMaxX && z >= out.window.originZ && z <= winMaxZ) {
        const f32 d = bilinearWorld(out.window, out.pierceDelta, x, z);
        if (d < -0.01f) {
            g += d;
        }
    }
    return g;
}

// Lateral re-centering: the window routed ~20% of points along
// fine-terrain walls — the axis ground sits metres above the
// groove the footprint min finds, so the course is buried and
// its water invisible. Each point drifts toward the LOWEST
// fine sample of its own footprint, in small SMOOTHED steps
// (a raw 1-step snap crenellated the polylines — v98). Ends,
// junction tails and points under a sheet stay put. The gate
// is per-river and HARD: if any turn ends sharper than 45°,
// the river's shift is rolled back whole.
void HydroReleveling::recenterCourses() {
    constexpr i32 kRecenterPasses = 10;
    constexpr f32 kRecenterStep = 4.0f; // m per smoothed pass
    constexpr f32 kKinkGateCos = 0.70710678f; // 45 degrees
    vector<f32> prePos;
    for (const River& river : rivers) {
        for (const RiverPoint& pt : river.points) {
            prePos.push_back(pt.x);
            prePos.push_back(pt.z);
        }
    }
    const auto underAnyLake = [&](f32 x, f32 z) {
        for (const Lake& lake : out.hydro.lakes) {
            if (lake.covers(x, z)) {
                return true;
            }
        }
        return false;
    };
    vector<f32> want;
    vector<f32> smooth;
    vector<f32> backupX;
    vector<f32> backupZ;
    for (River& river : rivers) {
        const size_t count = river.points.size();
        if (count < 9) {
            continue;
        }
        backupX.resize(count);
        backupZ.resize(count);
        for (size_t i = 0; i < count; ++i) {
            backupX[i] = river.points[i].x;
            backupZ[i] = river.points[i].z;
        }
        // The perpendicular frame is FROZEN on the original
        // polyline: recomputing it from already-shifted
        // neighbours fed the shift back into the frame and
        // folded the line into hairpins (measured: 174/176
        // rivers rolled back by the kink gate).
        vector<f32> perpX(count, 0.0f);
        vector<f32> perpZ(count, 0.0f);
        for (size_t i = 1; i + 1 < count; ++i) {
            const f32 dx = backupX[i + 1] - backupX[i - 1];
            const f32 dz = backupZ[i + 1] - backupZ[i - 1];
            const f32 len = std::sqrt(dx * dx + dz * dz);
            if (len > 0.5f) {
                perpX[i] = -dz / len;
                perpZ[i] = dx / len;
            }
        }
        for (i32 pass = 0; pass < kRecenterPasses; ++pass) {
            want.assign(count, 0.0f);
            for (size_t i = 3; i + 3 < count; ++i) {
                const RiverPoint& pt = river.points[i];
                if (underAnyLake(pt.x, pt.z)) {
                    continue;
                }
                const f32 px = perpX[i];
                const f32 pz = perpZ[i];
                if (px == 0.0f && pz == 0.0f) {
                    continue;
                }
                const f32 axisG = fineAt(pt.x, pt.z);
                f32 bestG = axisG;
                f32 bestD = 0.0f;
                for (const f32 off :
                     { -1.4f, -1.2f, -1.0f, -0.75f, -0.5f,
                       0.5f, 0.75f, 1.0f, 1.2f, 1.4f }) {
                    const f32 d = off * pt.halfWidth;
                    const f32 g = fineAt(pt.x + px * d,
                                         pt.z + pz * d);
                    if (g < bestG - 0.01f) {
                        bestG = g;
                        bestD = d;
                    }
                }
                // Converge instead of marching: only a point
                // whose AXIS stands clearly above its groove
                // moves (the buried family), and the pull
                // fades out as it reaches the floor — on a
                // plain monotone flank the min sits forever
                // at the footprint edge and an ungated pull
                // walked the course downhill without end.
                const f32 gain = glm::clamp(
                    (axisG - bestG - 1.0f) / 3.0f, 0.0f,
                    1.0f);
                want[i] = bestD * gain;
            }
            // Wide smoothing (~+-25 m): sign flips in the
            // raw targets are what crenellates the line.
            smooth = want;
            for (i32 s = 0; s < 3; ++s) {
                vector<f32> next = smooth;
                for (size_t i = 3; i + 3 < count; ++i) {
                    f32 acc = 0.0f;
                    for (i32 o = -3; o <= 3; ++o) {
                        acc += smooth[i + o];
                    }
                    next[i] = acc / 7.0f;
                }
                smooth.swap(next);
            }
            for (size_t i = 3; i + 3 < count; ++i) {
                RiverPoint& pt = river.points[i];
                const f32 step =
                    glm::clamp(smooth[i], -kRecenterStep,
                               kRecenterStep);
                pt.x += perpX[i] * step;
                pt.z += perpZ[i] * step;
            }
        }
        // Kink gate, SURGICAL: only the points around a
        // too-sharp turn fall back to the original line (the
        // restored set grows monotonically, so the loop
        // terminates — worst case the whole river reverts).
        // An all-or-nothing gate threw away every clean reach
        // of a river for one fold (90/176 rivers, measured).
        for (i32 guard = 0; guard < 64; ++guard) {
            bool dirty = false;
            for (size_t i = 1; i + 1 < count; ++i) {
                const f32 ax = river.points[i].x -
                               river.points[i - 1].x;
                const f32 az = river.points[i].z -
                               river.points[i - 1].z;
                const f32 bx2 = river.points[i + 1].x -
                                river.points[i].x;
                const f32 bz2 = river.points[i + 1].z -
                                river.points[i].z;
                const f32 la = std::sqrt(ax * ax + az * az);
                const f32 lb =
                    std::sqrt(bx2 * bx2 + bz2 * bz2);
                if (la < 0.1f || lb < 0.1f) {
                    continue;
                }
                if ((ax * bx2 + az * bz2) / (la * lb) >=
                    kKinkGateCos) {
                    continue;
                }
                bool changed = false;
                for (size_t k = (i >= 4 ? i - 4 : 0);
                     k <= glm::min(i + 4, count - 1); ++k) {
                    if (river.points[k].x != backupX[k] ||
                        river.points[k].z != backupZ[k]) {
                        river.points[k].x = backupX[k];
                        river.points[k].z = backupZ[k];
                        changed = true;
                    }
                }
                dirty = dirty || changed;
            }
            if (!dirty) {
                break;
            }
        }
        // Fords were snapped to the old axis: follow it.
        for (Vec2& ford : river.fords) {
            f32 best = 1.0e18f;
            size_t bi = 0;
            for (size_t i = 0; i < count; ++i) {
                const f32 fx = river.points[i].x - ford.x;
                const f32 fz = river.points[i].z - ford.y;
                const f32 d2 = fx * fx + fz * fz;
                if (d2 < best) {
                    best = d2;
                    bi = i;
                }
            }
            ford.x = river.points[bi].x;
            ford.y = river.points[bi].z;
        }
    }
    size_t pre = 0;
    for (const River& river : rivers) {
        for (const RiverPoint& pt : river.points) {
            if (pt.x != prePos[pre] || pt.z != prePos[pre + 1]) {
                ++stats.recenterMoved;
            }
            pre += 2;
        }
    }
}

// Corridor retrace: a course the window routed along a fine
// RIDGE sits metres above the groove of its own footprint —
// the nudging re-center cannot cross 40+ m, so the reach stays
// buried and its ribbon reads as a HOLE in the river (the
// dev's standing complaint). Each buried run is REPLACED by
// the lowest-saddle path on the 8 m fine lattice (Dijkstra,
// cost = climbs + a touch of distance), confined to a corridor
// around the original chain, endpoints fixed, resampled to the
// same point count, locally relaxed; a run whose path folds
// past 45° rolls back alone.
void HydroReleveling::retraceBuriedRuns() {
    constexpr f32 kBuriedRise = 4.0f;  // axis above footprint
    constexpr size_t kBuriedRun = 2;   // points to qualify
    constexpr f32 kCorridor = 110.0f;  // half-width of search
    constexpr f32 kCellT = 8.0f;
    constexpr f32 kKinkGateCos = 0.70710678f;
    for (size_t r = 0; r < riverCount; ++r) {
        River& river = rivers[r];
        const size_t count = river.points.size();
        if (count < kBuriedRun + 6) {
            continue;
        }
        // Buried flags from path geometry alone (the profile
        // does not exist yet): axis ground far above the
        // lowest fine sample of the local footprint.
        vector<u8> buried(count, 0);
        for (size_t i = 1; i + 1 < count; ++i) {
            const RiverPoint& pt = river.points[i];
            const f32 axis = fineAt(pt.x, pt.z);
            f32 lo = axis;
            const RiverPoint& fw = river.points[i + 1];
            const RiverPoint& bw = river.points[i - 1];
            const f32 dx = fw.x - bw.x;
            const f32 dz = fw.z - bw.z;
            const f32 len = std::sqrt(dx * dx + dz * dz);
            if (len < 0.5f) {
                continue;
            }
            const f32 px = -dz / len;
            const f32 pz = dx / len;
            for (const f32 off : { -1.4f, -0.7f, 0.7f, 1.4f }) {
                const f32 d = off * pt.halfWidth;
                lo = glm::min(lo, fineAt(pt.x + px * d,
                                         pt.z + pz * d));
            }
            buried[i] = axis > lo + kBuriedRise ? 1 : 0;
        }
        size_t i = 1;
        while (i + 1 < count) {
            if (!buried[i]) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j + 1 < count && buried[j + 1]) {
                ++j;
            }
            if (j - i + 1 < kBuriedRun || i < 1 ||
                j + 1 >= count) {
                i = j + 1;
                continue;
            }
            // Anchors just outside the buried run.
            const size_t ia = i - 1;
            const size_t ib = j + 1;
            const f32 ax = river.points[ia].x;
            const f32 az = river.points[ia].z;
            const f32 bx = river.points[ib].x;
            const f32 bz = river.points[ib].z;
            // Corridor grid around the run's bbox.
            f32 mnx = glm::min(ax, bx);
            f32 mxx = glm::max(ax, bx);
            f32 mnz = glm::min(az, bz);
            f32 mxz = glm::max(az, bz);
            for (size_t k = i; k <= j; ++k) {
                mnx = glm::min(mnx, river.points[k].x);
                mxx = glm::max(mxx, river.points[k].x);
                mnz = glm::min(mnz, river.points[k].z);
                mxz = glm::max(mxz, river.points[k].z);
            }
            const i32 gx0 = static_cast<i32>(
                std::floor((mnx - kCorridor) / kCellT));
            const i32 gx1 = static_cast<i32>(
                std::ceil((mxx + kCorridor) / kCellT));
            const i32 gz0 = static_cast<i32>(
                std::floor((mnz - kCorridor) / kCellT));
            const i32 gz1 = static_cast<i32>(
                std::ceil((mxz + kCorridor) / kCellT));
            const i32 gw = gx1 - gx0 + 1;
            const i32 gh = gz1 - gz0 + 1;
            if (gw <= 2 || gh <= 2 ||
                static_cast<i64>(gw) * gh > 400000) {
                i = j + 1;
                continue;
            }
            const auto cellOf = [&](f32 x, f32 z) {
                return std::pair<i32, i32>(
                    static_cast<i32>(
                        std::floor(x / kCellT)),
                    static_cast<i32>(
                        std::floor(z / kCellT)));
            };
            const auto idxOf = [&](i32 cx, i32 cz) {
                return static_cast<size_t>(cz - gz0) * gw +
                       static_cast<size_t>(cx - gx0);
            };
            const auto [sx, sz] = cellOf(ax, az);
            const auto [tx2, tz2] = cellOf(bx, bz);
            // Dijkstra: cost = uphill climbed + 0.02/m, so the
            // path hugs the grooves and still reaches the far
            // anchor.
            vector<f32> dist(static_cast<size_t>(gw) * gh,
                             1.0e18f);
            vector<i32> prev(static_cast<size_t>(gw) * gh,
                             -1);
            struct QC {
                f32 c;
                i32 cx;
                i32 cz;
                bool operator>(const QC& o) const {
                    return c > o.c;
                }
            };
            std::priority_queue<QC, vector<QC>,
                                std::greater<QC>>
                pq;
            dist[idxOf(sx, sz)] = 0.0f;
            pq.push({ 0.0f, sx, sz });
            const auto hAt = [&](i32 cx, i32 cz) {
                return fineAt(
                    static_cast<f32>(cx) * kCellT + 4.0f,
                    static_cast<f32>(cz) * kCellT + 4.0f);
            };
            bool reached = false;
            while (!pq.empty()) {
                const QC cur = pq.top();
                pq.pop();
                if (cur.cx == tx2 && cur.cz == tz2) {
                    reached = true;
                    break;
                }
                const size_t ci = idxOf(cur.cx, cur.cz);
                if (cur.c > dist[ci] + 1.0e-6f) {
                    continue;
                }
                const f32 hc = hAt(cur.cx, cur.cz);
                const i32 nb[4][2] = { { 1, 0 },
                                       { -1, 0 },
                                       { 0, 1 },
                                       { 0, -1 } };
                for (const auto& d : nb) {
                    const i32 nx = cur.cx + d[0];
                    const i32 nz = cur.cz + d[1];
                    if (nx < gx0 || nx > gx1 || nz < gz0 ||
                        nz > gz1) {
                        continue;
                    }
                    const size_t ni = idxOf(nx, nz);
                    const f32 climb = glm::max(
                        hAt(nx, nz) - hc, 0.0f);
                    const f32 nc =
                        cur.c + climb + 0.02f * kCellT;
                    if (nc < dist[ni] - 1.0e-6f) {
                        dist[ni] = nc;
                        prev[ni] = static_cast<i32>(ci);
                        pq.push({ nc, nx, nz });
                    }
                }
            }
            if (!reached) {
                i = j + 1;
                continue;
            }
            // Walk the path back, then resample it onto the
            // run's own point count (anchors excluded).
            vector<f32> pxs;
            vector<f32> pzs;
            i32 ci = static_cast<i32>(idxOf(tx2, tz2));
            while (ci >= 0) {
                const i32 cx =
                    gx0 + ci % gw;
                const i32 cz =
                    gz0 + ci / gw;
                pxs.push_back(
                    static_cast<f32>(cx) * kCellT + 4.0f);
                pzs.push_back(
                    static_cast<f32>(cz) * kCellT + 4.0f);
                ci = prev[ci];
            }
            std::reverse(pxs.begin(), pxs.end());
            std::reverse(pzs.begin(), pzs.end());
            if (pxs.size() < 2) {
                i = j + 1;
                continue;
            }
            pxs.front() = ax;
            pzs.front() = az;
            pxs.back() = bx;
            pzs.back() = bz;
            f32 total = 0.0f;
            vector<f32> arcs(pxs.size(), 0.0f);
            for (size_t k = 1; k < pxs.size(); ++k) {
                total += std::hypot(pxs[k] - pxs[k - 1],
                                    pzs[k] - pzs[k - 1]);
                arcs[k] = total;
            }
            vector<f32> backupX(count);
            vector<f32> backupZ(count);
            for (size_t k = 0; k < count; ++k) {
                backupX[k] = river.points[k].x;
                backupZ[k] = river.points[k].z;
            }
            const size_t n = j - i + 1;
            size_t seg = 0;
            for (size_t k = 0; k < n; ++k) {
                const f32 want =
                    total * static_cast<f32>(k + 1) /
                    static_cast<f32>(n + 1);
                while (seg + 1 < arcs.size() &&
                       arcs[seg + 1] < want) {
                    ++seg;
                }
                const size_t nx2 =
                    glm::min(seg + 1, arcs.size() - 1);
                const f32 span = glm::max(
                    arcs[nx2] - arcs[seg], 1.0e-3f);
                const f32 t =
                    glm::clamp((want - arcs[seg]) / span,
                               0.0f, 1.0f);
                river.points[i + k].x =
                    glm::mix(pxs[seg], pxs[nx2], t);
                river.points[i + k].z =
                    glm::mix(pzs[seg], pzs[nx2], t);
            }
            // Local relax (ends held by the anchors).
            for (i32 pass = 0; pass < 1; ++pass) {
                for (size_t k = i; k <= j; ++k) {
                    river.points[k].x =
                        0.5f * river.points[k].x +
                        0.25f * (river.points[k - 1].x +
                                 river.points[k + 1].x);
                    river.points[k].z =
                        0.5f * river.points[k].z +
                        0.25f * (river.points[k - 1].z +
                                 river.points[k + 1].z);
                }
            }
            // Kink gate on the touched span (one point of
            // margin): fold -> roll back this run only.
            bool folded = false;
            for (size_t k = glm::max(i - 1, size_t(1));
                 k <= glm::min(j + 1, count - 2); ++k) {
                const f32 ux = river.points[k].x -
                               river.points[k - 1].x;
                const f32 uz = river.points[k].z -
                               river.points[k - 1].z;
                const f32 vx = river.points[k + 1].x -
                               river.points[k].x;
                const f32 vz = river.points[k + 1].z -
                               river.points[k].z;
                const f32 lu = std::sqrt(ux * ux + uz * uz);
                const f32 lv = std::sqrt(vx * vx + vz * vz);
                if (lu < 0.1f || lv < 0.1f) {
                    continue;
                }
                if ((ux * vx + uz * vz) / (lu * lv) <
                    kKinkGateCos) {
                    folded = true;
                    break;
                }
            }
            if (folded) {
                for (size_t k = 0; k < count; ++k) {
                    river.points[k].x = backupX[k];
                    river.points[k].z = backupZ[k];
                }
            } else {
                ++stats.retracedRuns;
            }
            i = j + 1;
        }
    }
}

// Window-lake re-seat: an accepted lake's spill level is a
// 16 m estimate — containment the window guarantees can leak
// at 2 m through a sub-cell notch, and the rivers now seated
// on the fine floor expose every such overstatement (sheets
// hanging over their own shores). Each natural lake is
// priority-flooded on the 8 m lattice from its own wet cells:
// an escape below level - 0.1 is the TRUE spill — the level
// drops to it and the mask sheds the cells the lower sheet no
// longer holds. A lake touching the window edge is left alone
// (its basin is not fully visible here).
void HydroReleveling::reseatWindowLakes() {
    constexpr f32 kLakeTexel = 8.0f;
    constexpr f32 kLakeReach = 320.0f;
    for (Lake& lake : out.hydro.lakes) {
        if (lake.dug || lake.mask.empty() ||
            lake.maskWidth < 2) {
            continue;
        }
        const i32 gx0 = static_cast<i32>(std::floor(
            (lake.minX - kLakeReach) / kLakeTexel));
        const i32 gx1 = static_cast<i32>(std::ceil(
            (lake.maxX + kLakeReach) / kLakeTexel));
        const i32 gz0 = static_cast<i32>(std::floor(
            (lake.minZ - kLakeReach) / kLakeTexel));
        const i32 gz1 = static_cast<i32>(std::ceil(
            (lake.maxZ + kLakeReach) / kLakeTexel));
        if (static_cast<f32>(gx0) * kLakeTexel <
                out.window.originX ||
            static_cast<f32>(gx1) * kLakeTexel > winMaxX ||
            static_cast<f32>(gz0) * kLakeTexel <
                out.window.originZ ||
            static_cast<f32>(gz1) * kLakeTexel > winMaxZ) {
            continue;
        }
        const i32 gw = gx1 - gx0 + 1;
        const i32 gh = gz1 - gz0 + 1;
        vector<u8> seen(static_cast<size_t>(gw) * gh, 0);
        struct LakeCell {
            f32 ground;
            i32 cx;
            i32 cz;
            bool operator>(const LakeCell& o) const {
                return ground > o.ground;
            }
        };
        std::priority_queue<LakeCell, vector<LakeCell>,
                            std::greater<LakeCell>>
            heap;
        const f32 level = lake.level;
        const auto push = [&](i32 cx, i32 cz) {
            if (cx < gx0 || cx > gx1 || cz < gz0 ||
                cz > gz1) {
                return;
            }
            const size_t idx =
                static_cast<size_t>(cz - gz0) * gw +
                static_cast<size_t>(cx - gx0);
            if (seen[idx]) {
                return;
            }
            seen[idx] = 1;
            const f32 g =
                fineAt(static_cast<f32>(cx) * kLakeTexel,
                       static_cast<f32>(cz) * kLakeTexel);
            if (g >= level - 0.1f) {
                return;
            }
            heap.push({ g, cx, cz });
        };
        for (u32 mr = 0; mr < lake.maskHeight; ++mr) {
            for (u32 mc = 0; mc < lake.maskWidth; ++mc) {
                if (!lake.mask[static_cast<size_t>(mr) *
                                   lake.maskWidth +
                               mc]) {
                    continue;
                }
                const f32 px =
                    lake.minX +
                    static_cast<f32>(mc) * lake.maskTexel;
                const f32 pz =
                    lake.minZ +
                    static_cast<f32>(mr) * lake.maskTexel;
                push(static_cast<i32>(
                         std::floor(px / kLakeTexel)),
                     static_cast<i32>(
                         std::floor(pz / kLakeTexel)));
            }
        }
        f32 spill = level;
        bool escaped = false;
        while (!heap.empty()) {
            const LakeCell c = heap.top();
            heap.pop();
            if (c.cx == gx0 || c.cx == gx1 || c.cz == gz0 ||
                c.cz == gz1) {
                spill = c.ground;
                escaped = true;
                break;
            }
            push(c.cx - 1, c.cz);
            push(c.cx + 1, c.cz);
            push(c.cx, c.cz - 1);
            push(c.cx, c.cz + 1);
        }
        if (!escaped || spill >= level - 0.5f) {
            continue; // contained (or within the window's due)
        }
        lake.level =
            glm::max(spill - 0.1f, hydrology.seaLevel);
        // Shed the mask cells the lower sheet no longer
        // holds (the dig gate's own rule), keep the count
        // honest.
        lake.cells = 0;
        for (u32 mr = 0; mr < lake.maskHeight; ++mr) {
            for (u32 mc = 0; mc < lake.maskWidth; ++mc) {
                u8& wet =
                    lake.mask[static_cast<size_t>(mr) *
                                  lake.maskWidth +
                              mc];
                if (!wet) {
                    continue;
                }
                const f32 px =
                    lake.minX +
                    static_cast<f32>(mc) * lake.maskTexel;
                const f32 pz =
                    lake.minZ +
                    static_cast<f32>(mr) * lake.maskTexel;
                if (fineAt(px, pz) > lake.level + 0.5f) {
                    wet = 0;
                } else {
                    ++lake.cells;
                }
            }
        }
    }
}

// The per-course envelopes of the profile rebuild (frozen from here
// on — see the struct invariant): the 5-point footprint floor, the
// slope-limited fill ceiling, the covering lake level and the routed
// surface the seam softening blends back to.
void HydroReleveling::buildEnvelopes() {
    floors.resize(riverCount);
    bounds.resize(riverCount);
    // Per point, the level of the accepted lake covering it (or
    // far below): a course traced THROUGH a lake must never be
    // seated under the lake's own sill — the fill's ceiling came
    // from the lake BED, tunnelling the course metres under the
    // outlet and letting S5c slot a trench through the shore (the
    // "glass dam"). Inside a sheet, the water is the sheet.
    lakeLvls.resize(riverCount);
    oldSurfaces.resize(riverCount);
    const auto coveringLakeLevel = [&](f32 x, f32 z) -> f32 {
        for (const Lake& lake : out.hydro.lakes) {
            if (!lake.dug && lake.covers(x, z)) {
                return lake.level;
            }
        }
        return -1.0e18f;
    };
    vector<f32> raw;
    for (size_t r = 0; r < riverCount; ++r) {
        const River& river = rivers[r];
        const size_t count = river.points.size();
        raw.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const RiverPoint& pt = river.points[i];
            f32 floor = fineAt(pt.x, pt.z);
            const RiverPoint& fw =
                river.points[i + 1 < count ? i + 1 : i];
            const RiverPoint& bw = river.points[i > 0 ? i - 1 : i];
            const f32 dx = fw.x - bw.x;
            const f32 dz = fw.z - bw.z;
            const f32 len = std::sqrt(dx * dx + dz * dz);
            if (len > 0.5f) {
                const f32 px = -dz / len;
                const f32 pz = dx / len;
                for (const f32 off :
                     { -1.4f, -1.2f, -1.0f, -0.75f, -0.5f, 0.5f,
                       0.75f, 1.0f, 1.2f, 1.4f }) {
                    const f32 d = off * pt.halfWidth;
                    floor = glm::min(
                        floor, fineAt(pt.x + px * d, pt.z + pz * d));
                }
            }
            raw[i] = floor;
        }
        vector<f32>& f3 = floors[r];
        f3.resize(count);
        for (size_t i = 0; i < count; ++i) {
            f32 lo = raw[i];
            for (size_t k = (i >= 2 ? i - 2 : 0);
                 k <= glm::min(i + 2, count - 1); ++k) {
                lo = glm::min(lo, raw[k]);
            }
            f3[i] = lo;
        }
        // Forward slope-limited envelope of floor + pond depth:
        // the fill's ceiling. min() keeps it at the local floor's
        // pond level; the recovery term lets it climb back only
        // gently after a low stretch, so the tunnel under a rising
        // ridge holds for the ridge's whole length.
        vector<f32>& bound = bounds[r];
        bound.resize(count);
        if (count > 0) {
            bound[0] = f3[0] + kPoolMax;
            for (size_t i = 1; i < count; ++i) {
                const f32 ds = std::hypot(
                    river.points[i].x - river.points[i - 1].x,
                    river.points[i].z - river.points[i - 1].z);
                bound[i] =
                    glm::min(f3[i] + kPoolMax,
                             bound[i - 1] + kBoundRecover * ds);
            }
        }
        vector<f32>& lv = lakeLvls[r];
        lv.resize(count);
        for (size_t i = 0; i < count; ++i) {
            lv[i] = coveringLakeLevel(river.points[i].x,
                                      river.points[i].z);
        }
        vector<f32>& olds = oldSurfaces[r];
        olds.resize(count);
        for (size_t i = 0; i < count; ++i) {
            olds[i] = river.points[i].surface;
        }
    }
}

// Junction links (the data has none): a course's mouth within
// confluence reach of another course drains INTO it — its
// boundary level is the receiver's recomputed surface there,
// so tributaries land ON their stem instead of pooling from
// their own floor.
void HydroReleveling::linkTributaries() {
    links.assign(riverCount, JunctionLink {});
    for (size_t r = 0; r < riverCount; ++r) {
        if (rivers[r].points.empty()) {
            continue;
        }
        const RiverPoint& tail = rivers[r].points.back();
        f32 bestD2 = 1.0e18f;
        for (size_t o = 0; o < riverCount; ++o) {
            if (o == r || rivers[o].points.empty()) {
                continue;
            }
            const vector<RiverPoint>& opts = rivers[o].points;
            for (size_t j = 0; j < opts.size(); ++j) {
                const f32 reach = glm::max(
                    (tail.halfWidth + opts[j].halfWidth) * 2.2f,
                    15.0f);
                const f32 dx = opts[j].x - tail.x;
                const f32 dz = opts[j].z - tail.z;
                const f32 d2 = dx * dx + dz * dz;
                if (d2 < bestD2 && d2 <= reach * reach) {
                    bestD2 = d2;
                    links[r].receiver = static_cast<i32>(o);
                    links[r].recvIdx = j;
                }
            }
        }
    }
    // Receivers fill before their tributaries; a coincidental
    // proximity cycle drops the link whose mouth floor sits
    // highest (that course falls back to its own floor).
    vector<u8> filled(riverCount, 0);
    order.clear();
    order.reserve(riverCount);
    while (order.size() < riverCount) {
        bool progressed = false;
        for (size_t r = 0; r < riverCount; ++r) {
            if (filled[r]) {
                continue;
            }
            const i32 recv = links[r].receiver;
            if (recv < 0 || filled[static_cast<size_t>(recv)]) {
                filled[r] = 1;
                order.push_back(r);
                progressed = true;
            }
        }
        if (!progressed) {
            size_t cut = riverCount;
            f32 hi = -1.0e18f;
            for (size_t r = 0; r < riverCount; ++r) {
                if (filled[r] || links[r].receiver < 0) {
                    continue;
                }
                const f32 h = floors[r].empty() ? -1.0e18f
                                                : floors[r].back();
                if (cut == riverCount || h > hi) {
                    hi = h;
                    cut = r;
                }
            }
            if (cut == riverCount) {
                break;
            }
            links[cut].receiver = -1;
        }
    }
}

// The upstream pooling fill itself: receivers first (`order`), each
// course seated from its mouth boundary up, never above its ceiling
// nor under a covering sheet.
void HydroReleveling::fillProfiles() {
    for (const size_t r : order) {
        River& river = rivers[r];
        const size_t count = river.points.size();
        if (count == 0) {
            continue;
        }
        const vector<f32>& f3 = floors[r];
        const vector<f32>& bound = bounds[r];
        // Mouth boundary: the sea for estuaries (the routed trace
        // ended at a sea cell), the receiver's surface at a
        // junction, nothing at a window-border outlet, and a
        // 3-point median at a plain interior stop (one pit at the
        // very mouth must not sink the whole tail).
        const RiverPoint& tail = river.points.back();
        f32 bndry = -1.0e18f;
        const bool seaMouth =
            tail.surface <= hydrology.seaLevel + 0.5f;
        // Border-mouth detection band in world metres (a smoothed
        // mouth can sit a legacy cell off the window edge whatever
        // the routing texel).
        constexpr f32 kBorderBand = 16.0f;
        const bool borderMouth =
            tail.x <= out.window.originX + kBorderBand ||
            tail.x >= winMaxX - kBorderBand ||
            tail.z <= out.window.originZ + kBorderBand ||
            tail.z >= winMaxZ - kBorderBand;
        if (links[r].receiver >= 0) {
            bndry = rivers[static_cast<size_t>(links[r].receiver)]
                        .points[links[r].recvIdx]
                        .surface;
        } else if (!seaMouth && !borderMouth && count >= 3) {
            const f32 a = f3[count - 1];
            const f32 b = f3[count - 2];
            const f32 c = f3[count - 3];
            bndry = glm::max(glm::min(a, b),
                             glm::min(glm::max(a, b), c)) -
                    kEdgeTuck;
        }
        // The head of a lake outlet never rises above its lake:
        // pooling past the sill would put the river ABOVE the
        // sheet it springs from.
        const bool hasLake =
            river.lakeFed != 0 && river.sourceLake >= 0 &&
            static_cast<size_t>(river.sourceLake) <
                out.hydro.lakes.size();
        const f32 lakeLevel =
            hasLake ? out.hydro.lakes[static_cast<size_t>(
                                          river.sourceLake)]
                          .level
                    : 0.0f;
        vector<f32> headArc;
        if (hasLake) {
            headArc.resize(count, 0.0f);
            for (size_t k = 1; k < count; ++k) {
                headArc[k] =
                    headArc[k - 1] +
                    std::hypot(
                        river.points[k].x - river.points[k - 1].x,
                        river.points[k].z - river.points[k - 1].z);
            }
        }
        const vector<f32>& lv = lakeLvls[r];
        f32 level = glm::max(
            glm::max(glm::max(glm::min(f3[count - 1],
                                       bound[count - 1]),
                              lv[count - 1]) -
                         kEdgeTuck,
                     bndry),
            hydrology.seaLevel);
        stats.fillShift +=
            std::abs(level - river.points[count - 1].surface);
        river.points[count - 1].surface = level;
        for (size_t i = count - 1; i-- > 0;) {
            const f32 own =
                glm::max(glm::min(f3[i], bound[i]), lv[i]) -
                kEdgeTuck;
            f32 next = glm::max(own, level);
            if (hasLake && headArc[i] < 30.0f) {
                next = glm::min(next, glm::max(lakeLevel, level));
            }
            level = next;
            stats.fillShift += std::abs(level - river.points[i].surface);
            river.points[i].surface = level;
        }
    }
}

// Discovered pools: where the fill pooled the surface above
// the local floor, the real water body is WIDER than the
// ribbon — the far water paints it while the sim pin stops at
// the ribbon footprint (the dry-band defect, measured on
// ~half the network when pools stayed ribbon-only). A pooled
// reach IS a lake: it is emitted as a masked natural lake and
// the existing machinery owns it from there — S5d-bis digs
// the bed, the nappe renders it far and near, pinLakes
// reservoirs it, the ribbon is cut under it at emission.
void HydroReleveling::discoverPools() {
    constexpr f32 kPoolMinRise = 1.0f;
    constexpr size_t kPoolMinRun = 3;
    constexpr f32 kPoolMaskTexel = 8.0f;
    constexpr f32 kPoolMaxReach = 320.0f;
    constexpr u32 kPoolMinCells = 12;
    const auto lakeCovers = [](const Lake& lake, f32 x,
                               f32 z) {
        return lake.covers(x, z);
    };
    const size_t preExistingLakes = out.hydro.lakes.size();
    for (size_t r = 0; r < riverCount; ++r) {
        River& river = rivers[r];
        const size_t count = river.points.size();
        const vector<f32>& f3 = floors[r];
        const vector<f32>& bound = bounds[r];
        size_t i = 0;
        while (i < count) {
            if (river.points[i].surface - f3[i] <=
                kPoolMinRise) {
                ++i;
                continue;
            }
            const f32 level = river.points[i].surface;
            size_t j = i;
            while (j + 1 < count &&
                   river.points[j + 1].surface - f3[j + 1] >
                       kPoolMinRise &&
                   std::abs(river.points[j + 1].surface -
                            level) < 0.01f) {
                ++j;
            }
            const size_t runLen = j - i + 1;
            const f32 midX =
                river.points[(i + j) / 2].x;
            const f32 midZ =
                river.points[(i + j) / 2].z;
            bool inExisting = false;
            for (size_t l = 0; l < preExistingLakes; ++l) {
                if (lakeCovers(out.hydro.lakes[l], midX,
                               midZ)) {
                    inExisting = true;
                    break;
                }
            }
            if (runLen < kPoolMinRun || inExisting) {
                i = j + 1;
                continue;
            }
            // Priority-flood the pool's footprint on an 8 m
            // world-aligned lattice (deterministic and
            // seam-stable), bounded to the run's bbox + reach.
            // Cells flood in ascending ground order; reaching
            // the bound means the fine terrain offers a drain
            // the 16 m routing never saw — the pool is FALSE
            // above that saddle, so the water level is LOWERED
            // to it (real water would run out there) and only
            // the contained basin is kept.
            f32 rMinX = river.points[i].x;
            f32 rMaxX = rMinX;
            f32 rMinZ = river.points[i].z;
            f32 rMaxZ = rMinZ;
            for (size_t k = i; k <= j; ++k) {
                rMinX = glm::min(rMinX, river.points[k].x);
                rMaxX = glm::max(rMaxX, river.points[k].x);
                rMinZ = glm::min(rMinZ, river.points[k].z);
                rMaxZ = glm::max(rMaxZ, river.points[k].z);
            }
            const i32 gx0 = static_cast<i32>(std::floor(
                (rMinX - kPoolMaxReach) / kPoolMaskTexel));
            const i32 gx1 = static_cast<i32>(std::ceil(
                (rMaxX + kPoolMaxReach) / kPoolMaskTexel));
            const i32 gz0 = static_cast<i32>(std::floor(
                (rMinZ - kPoolMaxReach) / kPoolMaskTexel));
            const i32 gz1 = static_cast<i32>(std::ceil(
                (rMaxZ + kPoolMaxReach) / kPoolMaskTexel));
            const i32 gw = gx1 - gx0 + 1;
            const i32 gh = gz1 - gz0 + 1;
            vector<u8> seen(static_cast<size_t>(gw) * gh, 0);
            struct PoolCell {
                f32 ground;
                i32 cx;
                i32 cz;
                bool operator>(const PoolCell& o) const {
                    return ground > o.ground;
                }
            };
            std::priority_queue<PoolCell, vector<PoolCell>,
                                std::greater<PoolCell>>
                heap;
            const auto push = [&](i32 cx, i32 cz) {
                if (cx < gx0 || cx > gx1 || cz < gz0 ||
                    cz > gz1) {
                    return;
                }
                const size_t idx =
                    static_cast<size_t>(cz - gz0) * gw +
                    static_cast<size_t>(cx - gx0);
                if (seen[idx]) {
                    return;
                }
                seen[idx] = 1;
                const f32 g = fineAt(
                    static_cast<f32>(cx) * kPoolMaskTexel,
                    static_cast<f32>(cz) * kPoolMaskTexel);
                if (g >= level - 0.1f) {
                    return;
                }
                heap.push({ g, cx, cz });
            };
            for (size_t k = i; k <= j; ++k) {
                push(static_cast<i32>(std::floor(
                         river.points[k].x / kPoolMaskTexel)),
                     static_cast<i32>(std::floor(
                         river.points[k].z /
                         kPoolMaskTexel)));
            }
            vector<PoolCell> popped;
            f32 spill = level;
            bool escaped = false;
            while (!heap.empty()) {
                const PoolCell c = heap.top();
                heap.pop();
                if (c.cx == gx0 || c.cx == gx1 ||
                    c.cz == gz0 || c.cz == gz1) {
                    // The lowest way out: everything above
                    // this saddle drains through it.
                    spill = c.ground;
                    escaped = true;
                    break;
                }
                popped.push_back(c);
                push(c.cx - 1, c.cz);
                push(c.cx + 1, c.cz);
                push(c.cx, c.cz - 1);
                push(c.cx, c.cz + 1);
            }
            const f32 poolLevel =
                escaped ? spill - 0.1f : level;
            if (escaped) {
                // Re-seat the run on the corrected level: the
                // monotone max keeps the profile sound, the
                // pooled plateau simply sits lower.
                f32 lvl = river.points[j + 1 < count ? j + 1
                                                     : j]
                              .surface;
                if (j + 1 >= count) {
                    lvl = -1.0e18f;
                }
                const vector<f32>& lvv = lakeLvls[r];
                for (size_t k = j + 1; k-- > i;) {
                    const f32 own =
                        glm::max(glm::min(f3[k], bound[k]),
                                 lvv[k]) -
                        kEdgeTuck;
                    lvl = glm::max(
                        glm::max(own, glm::min(lvl,
                                               poolLevel)),
                        hydrology.seaLevel);
                    river.points[k].surface = lvl;
                }
                // Upstream of the run keeps monotonicity: its
                // levels were >= the old pool, which only
                // dropped.
                for (size_t k = i; k-- > 0;) {
                    if (river.points[k].surface >= lvl) {
                        break;
                    }
                    lvl = glm::max(river.points[k].surface,
                                   lvl);
                    river.points[k].surface = lvl;
                }
            }
            u32 cells = 0;
            i32 wMinX = gx1, wMaxX = gx0, wMinZ = gz1,
                wMaxZ = gz0;
            for (const PoolCell& c : popped) {
                if (c.ground >= poolLevel - 0.1f) {
                    continue;
                }
                ++cells;
                wMinX = glm::min(wMinX, c.cx);
                wMaxX = glm::max(wMaxX, c.cx);
                wMinZ = glm::min(wMinZ, c.cz);
                wMaxZ = glm::max(wMaxZ, c.cz);
            }
            if (cells >= kPoolMinCells) {
                // One dry ring around the covered cells: the
                // mask border is shore (S5d-bis and the mesh
                // perimeter probes both expect it).
                Lake pool;
                pool.dug = 0;
                pool.level = poolLevel;
                pool.cells = cells;
                pool.catchment = river.points[j].area;
                pool.maskTexel = kPoolMaskTexel;
                pool.minX = static_cast<f32>(wMinX - 1) *
                            kPoolMaskTexel;
                pool.minZ = static_cast<f32>(wMinZ - 1) *
                            kPoolMaskTexel;
                pool.maxX = static_cast<f32>(wMaxX + 1) *
                            kPoolMaskTexel;
                pool.maxZ = static_cast<f32>(wMaxZ + 1) *
                            kPoolMaskTexel;
                pool.maskWidth =
                    static_cast<u32>(wMaxX - wMinX + 3);
                pool.maskHeight =
                    static_cast<u32>(wMaxZ - wMinZ + 3);
                pool.mask.assign(
                    static_cast<size_t>(pool.maskWidth) *
                        pool.maskHeight,
                    0);
                for (const PoolCell& c : popped) {
                    if (c.ground >= poolLevel - 0.1f) {
                        continue;
                    }
                    pool.mask[static_cast<size_t>(
                                  c.cz - wMinZ + 1) *
                                  pool.maskWidth +
                              static_cast<size_t>(
                                  c.cx - wMinX + 1)] = 1;
                }
                out.hydro.lakes.push_back(std::move(pool));
            }
            i = j + 1;
        }
    }
    stats.poolLakes = out.hydro.lakes.size() - preExistingLakes;
}

// Cascade coalescing: a real benched descent (lake-outlet
// pierce ramps, terraced gorges) re-emerges from the fine
// floor as stacked 5-10 m lips with 8-25 m treads — a
// staircase of slabs. Successive falls separated by SHORT
// treads are merged into ONE steep ramp from the cluster's
// top to its bottom: the carve digs a continuous chute
// through the benches, and the shader already dresses steep
// ribbons as whitewater. Endpoints keep their levels, so
// monotonicity is untouched. Runs AFTER pool discovery (the
// spill-lowering must see the un-ramped flats) and never
// ramps through a point a nappe covers: a lake pour-over
// stays a single frank lip from the sheet.
void HydroReleveling::coalesceCascades() {
    constexpr f32 kFallStep = 1.5f;  // a lip worth merging (m)
    constexpr f32 kTreadMax = 30.0f; // tread that still chains
    const auto underLake = [&](const RiverPoint& pt) {
        for (const Lake& lake : out.hydro.lakes) {
            if (lake.covers(pt.x, pt.z)) {
                return true;
            }
        }
        return false;
    };
    for (River& river : rivers) {
        const size_t count = river.points.size();
        if (count < 3) {
            continue;
        }
        vector<u8> wet(count, 0);
        for (size_t k = 0; k < count; ++k) {
            wet[k] = underLake(river.points[k]) ? 1 : 0;
        }
        vector<f32> arc(count, 0.0f);
        for (size_t k = 1; k < count; ++k) {
            arc[k] = arc[k - 1] +
                     std::hypot(river.points[k].x -
                                    river.points[k - 1].x,
                                river.points[k].z -
                                    river.points[k - 1].z);
        }
        size_t k = 0;
        while (k + 1 < count) {
            if (wet[k] || wet[k + 1] ||
                river.points[k].surface -
                        river.points[k + 1].surface <
                    kFallStep) {
                ++k;
                continue;
            }
            // Cluster start: chain further lips while treads
            // stay short.
            const size_t top = k;
            size_t bottom = k + 1;
            size_t lastLipEnd = k + 1;
            size_t lips = 1;
            size_t m = k + 1;
            while (m + 1 < count && !wet[m + 1] &&
                   arc[m + 1] - arc[lastLipEnd] <= kTreadMax) {
                if (river.points[m].surface -
                        river.points[m + 1].surface >=
                    kFallStep) {
                    bottom = m + 1;
                    lastLipEnd = m + 1;
                    ++lips;
                }
                ++m;
            }
            if (lips >= 2) {
                const f32 sTop = river.points[top].surface;
                const f32 sBot = river.points[bottom].surface;
                const f32 span =
                    glm::max(arc[bottom] - arc[top], 0.001f);
                for (size_t q = top + 1; q < bottom; ++q) {
                    river.points[q].surface =
                        sTop + (sBot - sTop) *
                                   (arc[q] - arc[top]) / span;
                }
            }
            k = bottom;
        }
    }
}

// Slope cap: a lip steeper than rapids is tilted INTO the
// ground — the approach above a drop slopes down at the cap
// and the carve digs the chute through the lip, so the water
// descends as a visible rapid instead of a stair of slabs
// (dev direction: slopes show the simulated water, steps
// hide it). The cap never pulls a point under a crossed
// lake's sheet (the anti-tunnel rule holds).
void HydroReleveling::capSlopes() {
    // Dev direction (after seeing 0.4): long gentle glides,
    // and FRANK falls where the terrain truly cliffs — the
    // tilt floors below turn anything steeper than the cap +
    // 6 m of cut into a vertical drop with its spray, instead
    // of a continuum of steep ramps.
    constexpr f32 kMaxDownSlope = 0.18f; // m per m of course
    // The tilt may dive at most this far under the local fine
    // floor: within the S5c gate's reach, so the chute is
    // actually DUG — an unbounded tilt buried whole
    // approaches under lips taller than the gate (enterres
    // 13.6 -> 30.1 %, measured).
    constexpr f32 kTiltCut = 6.0f;
    for (size_t r = 0; r < riverCount; ++r) {
        River& river = rivers[r];
        const size_t count = river.points.size();
        if (count < 2) {
            continue;
        }
        const vector<f32>& lv = lakeLvls[r];
        const vector<f32>& f5 = floors[r];
        for (size_t k = count - 1; k-- > 0;) {
            const f32 ds = glm::max(
                std::hypot(river.points[k + 1].x -
                               river.points[k].x,
                           river.points[k + 1].z -
                               river.points[k].z),
                0.001f);
            f32 capped = glm::min(
                river.points[k].surface,
                river.points[k + 1].surface +
                    kMaxDownSlope * ds);
            capped = glm::max(
                glm::max(capped,
                         glm::max(f5[k] - kTiltCut,
                                  lv[k] - kEdgeTuck)),
                hydrology.seaLevel);
            // Never ABOVE the original profile: the floors
            // only bound the dive, they must not re-raise.
            capped = glm::min(capped,
                              river.points[k].surface);
            river.points[k].surface = capped;
        }
    }
}

// Longitudinal smoothing: on a slope steeper than the cap the
// profile was FLOOR-FOLLOWING (bounded 6 m under the benches)
// — quasi-flat treads alternating with 3-10 m plunges every
// node, which a wide ribbon renders as stacked terraces (the
// dev's "water staircases everywhere", measured on a 0.64
// bowl descent). Water running a steep reach cuts a STRAIGHT
// chute: the profile is box-smoothed along the course, riding
// over bench noses (the carve gate, 7.5-14 m, digs through
// them) and pooling at most lightly above the old line. Lake
// levels and the sea stay floors; monotonicity is re-enforced.
void HydroReleveling::smoothProfiles() {
    constexpr i32 kSmoothIters = 2;
    constexpr i32 kSmoothHalf = 5;    // ~±38 m of course
    constexpr f32 kSmoothRaise = 2.0f; // max rise over the old
    vector<f32> buf;
    for (size_t r = 0; r < riverCount; ++r) {
        River& river = rivers[r];
        const size_t count = river.points.size();
        if (count < static_cast<size_t>(2 * kSmoothHalf + 3)) {
            continue;
        }
        const vector<f32>& lv = lakeLvls[r];
        vector<f32> s(count);
        for (size_t i = 0; i < count; ++i) {
            s[i] = river.points[i].surface;
        }
        buf = s;
        for (i32 it = 0; it < kSmoothIters; ++it) {
            for (size_t i = kSmoothHalf;
                 i + kSmoothHalf < count; ++i) {
                f32 acc = 0.0f;
                for (i32 o = -kSmoothHalf; o <= kSmoothHalf;
                     ++o) {
                    acc += buf[i + o];
                }
                s[i] = acc /
                       static_cast<f32>(2 * kSmoothHalf + 1);
            }
            buf = s;
        }
        for (size_t i = 0; i < count; ++i) {
            f32 v = glm::min(
                s[i], river.points[i].surface + kSmoothRaise);
            v = glm::max(glm::max(v, lv[i] - kEdgeTuck),
                         hydrology.seaLevel);
            river.points[i].surface = v;
        }
        for (size_t i = count - 1; i-- > 0;) {
            river.points[i].surface =
                glm::max(river.points[i].surface,
                         river.points[i + 1].surface);
        }
    }
}

// Steep water runs NARROW (discharge continuity: what falls
// fast needs less section): the law width never read the
// final profile, so a 108 m fleuve could coil down a 62 m
// drop — ten stacked 150 m sheets at one spot (the dev's
// "gigantic water structure", measured at (4800, 4900)). The
// half-width shrinks with the FINAL water slope, floored at
// the law's own minimum.
void HydroReleveling::narrowSteep() {
    constexpr f32 kNarrowFrom = 0.08f; // slope where it starts
    constexpr f32 kNarrowTo = 0.30f;   // fully narrowed
    constexpr f32 kNarrowMin = 0.3f;   // keep 30 % at steepest
    for (size_t r = 0; r < riverCount; ++r) {
        River& river = rivers[r];
        const size_t count = river.points.size();
        if (count < 3) {
            continue;
        }
        for (size_t i = 0; i < count; ++i) {
            const size_t a = i >= 2 ? i - 2 : 0;
            const size_t b = glm::min(i + 2, count - 1);
            const f32 run = glm::max(
                std::hypot(river.points[b].x -
                               river.points[a].x,
                           river.points[b].z -
                               river.points[a].z),
                1.0f);
            const f32 slope =
                (river.points[a].surface -
                 river.points[b].surface) /
                run;
            const f32 t = glm::clamp(
                (slope - kNarrowFrom) /
                    (kNarrowTo - kNarrowFrom),
                0.0f, 1.0f);
            const f32 factor =
                1.0f - (1.0f - kNarrowMin) * t * t *
                           (3.0f - 2.0f * t);
            river.points[i].halfWidth = glm::max(
                river.points[i].halfWidth * factor, 1.2f);
        }
    }
}

// Tributaries were filled against their receiver's surface
// BEFORE the pool re-seat and the cascade coalescing moved it:
// a lowered stem left hanging lips at confluences. Re-clamp
// each linked course, receivers first — lowering ONLY, so the
// ramps and the re-seats the later passes produced survive.
void HydroReleveling::reclampTributaries() {
    for (const size_t r : order) {
        if (links[r].receiver < 0 || rivers[r].points.empty()) {
            continue;
        }
        River& river = rivers[r];
        const size_t count = river.points.size();
        const vector<f32>& f3 = floors[r];
        const vector<f32>& bound = bounds[r];
        const vector<f32>& lv = lakeLvls[r];
        f32 down =
            rivers[static_cast<size_t>(links[r].receiver)]
                .points[links[r].recvIdx]
                .surface;
        for (size_t k = count; k-- > 0;) {
            const f32 own =
                glm::max(glm::min(f3[k], bound[k]), lv[k]) -
                kEdgeTuck;
            const f32 cand = glm::max(
                glm::max(own, down), hydrology.seaLevel);
            if (river.points[k].surface <= cand + 0.001f) {
                break; // already at or under: upstream unchanged
            }
            river.points[k].surface = cand;
            down = cand;
        }
    }
}

// Seam softening: past the map rect, fineAt reads the
// NEIGHBOUR'S published ground — already carved — so the
// re-seat there dug under beds a second time and stepped the
// water at map lines. Blend the recomputed surface back to
// the routing surface over the overlap approach; the
// neighbour's own bake owns those points in ITS rect.
void HydroReleveling::softenMapSeams() {
    const f32 rectMinX =
        out.window.originX + params.waterMargin;
    const f32 rectMinZ =
        out.window.originZ + params.waterMargin;
    const f32 rectMaxX = winMaxX - params.waterMargin;
    const f32 rectMaxZ = winMaxZ - params.waterMargin;
    for (size_t r = 0; r < riverCount; ++r) {
        River& river = rivers[r];
        const vector<f32>& olds = oldSurfaces[r];
        for (size_t i = 0; i < river.points.size(); ++i) {
            RiverPoint& pt = river.points[i];
            const f32 dOut = glm::max(
                glm::max(rectMinX - pt.x, pt.x - rectMaxX),
                glm::max(rectMinZ - pt.z, pt.z - rectMaxZ));
            if (dOut <= 0.0f) {
                continue;
            }
            f32 w = glm::clamp(dOut / 96.0f, 0.0f, 1.0f);
            w = w * w * (3.0f - 2.0f * w);
            pt.surface =
                glm::mix(pt.surface, olds[i], w);
        }
    }
}

// Every dug pond was leveled from the pre-recompute surfaces
// (makePond: surface - 0.1) — hairpin ponds and orphaned
// junctions included, not just the re-linked tails: sweep ALL
// of them onto the lowest recomputed surface that actually
// runs through their footprint, so no pond disc floats above
// its own river.
void HydroReleveling::reseatPonds() {
    for (Lake& lake : out.hydro.lakes) {
        if (!lake.dug) {
            continue;
        }
        const f32 m = lake.maskTexel + 8.0f;
        f32 lo = 1.0e18f;
        for (const River& river : rivers) {
            for (const RiverPoint& pt : river.points) {
                if (pt.x < lake.minX - m || pt.x > lake.maxX + m ||
                    pt.z < lake.minZ - m ||
                    pt.z > lake.maxZ + m) {
                    continue;
                }
                lo = glm::min(lo, pt.surface);
            }
        }
        if (lo < 1.0e17f) {
            lake.level = glm::min(lake.level, lo - 0.1f);
        }
    }
}

// A ford on a rapids reach would cap the carve to a wading
// shelf mid-chute: fords stay on quiet water only.
void HydroReleveling::dropRapidsFords() {
    for (River& river : rivers) {
        if (river.fords.empty() || river.points.size() < 2) {
            continue;
        }
        const vector<RiverPoint>& pts = river.points;
        std::erase_if(river.fords, [&](const Vec2& ford) {
            for (size_t i = 0; i + 1 < pts.size(); ++i) {
                const f32 sx = pts[i + 1].x - pts[i].x;
                const f32 sz = pts[i + 1].z - pts[i].z;
                const f32 segLen2 =
                    glm::max(sx * sx + sz * sz, 1.0e-6f);
                const f32 slope =
                    (pts[i].surface - pts[i + 1].surface) /
                    std::sqrt(segLen2);
                if (slope <= 0.08f) {
                    continue;
                }
                const f32 t = glm::clamp(
                    ((ford.x - pts[i].x) * sx +
                     (ford.y - pts[i].z) * sz) /
                        segLen2,
                    0.0f, 1.0f);
                const f32 dx = ford.x - (pts[i].x + t * sx);
                const f32 dz = ford.y - (pts[i].z + t * sz);
                if (dx * dx + dz * dz < 12.0f * 12.0f) {
                    return true;
                }
            }
            return false;
        });
    }
}

// Saddle cut: a SHORT buried run (the course crosses a fine
// swell no other pass can fix — the hole-necklace read) gets
// the ground brought down to its water, with the pierce
// notch's own geometry (gentle V at the course gabarit, 0.7
// flanks — the lake-outlet gorges already use this language
// map-wide). Long or very deep runs stay buried: a cut there
// is a canyon trench through terrain the water should never
// have crossed. Dig-only, written through pierceDelta so the
// fine transfer and the S5c carve see it like any pierce.
void HydroReleveling::cutSaddles() {
    constexpr f32 kSaddleMaxLen = 160.0f;
    constexpr f32 kSaddleMaxExcess = 32.0f;
    constexpr f32 kSaddleBuried = 2.0f; // ribbon-buried gate
    constexpr f32 kSaddleFloor = 0.5f;  // notch under waterline
    const i32 n = static_cast<i32>(out.window.n);
    const auto underAcceptedLake = [&](f32 x, f32 z) {
        for (const Lake& lake : out.hydro.lakes) {
            if (lake.covers(x, z)) {
                return true;
            }
        }
        return false;
    };
    const auto notchAt = [&](const RiverPoint& pt) {
        const f32 level0 = pt.surface - kSaddleFloor;
        const f32 bandHalf =
            glm::clamp(pt.halfWidth * 1.6f, 6.0f, 48.0f);
        // Reach: band + enough flank to daylight the deepest
        // allowed cut at the 0.7 side slope.
        const f32 reach =
            bandHalf + kSaddleMaxExcess / 0.7f + 8.0f;
        const f32 texel = out.window.texelSize;
        const i32 cx = static_cast<i32>(
            std::floor((pt.x - out.window.originX) / texel));
        const i32 cz = static_cast<i32>(
            std::floor((pt.z - out.window.originZ) / texel));
        const i32 r =
            static_cast<i32>(std::ceil(reach / texel));
        for (i32 dz = -r; dz <= r; ++dz) {
            for (i32 dx = -r; dx <= r; ++dx) {
                const i32 px = cx + dx;
                const i32 pz = cz + dz;
                if (px < 0 || pz < 0 || px >= n || pz >= n) {
                    continue;
                }
                const size_t p =
                    static_cast<size_t>(pz) * out.window.n +
                    static_cast<size_t>(px);
                const f32 wx = out.window.x(
                    static_cast<u32>(px));
                const f32 wz = out.window.z(
                    static_cast<u32>(pz));
                const f32 dist =
                    std::hypot(wx - pt.x, wz - pt.z);
                const f32 inner = glm::min(dist, bandHalf);
                const f32 lateral =
                    glm::max(dist - bandHalf, 0.0f);
                const f32 target = level0 + 0.15f * inner +
                                   0.7f * lateral;
                const f32 delta = target - preGround[p];
                if (delta < out.pierceDelta[p]) {
                    out.pierceDelta[p] = delta;
                    out.ground[p] = preGround[p] + delta;
                }
            }
        }
    };
    for (size_t r = 0; r < riverCount; ++r) {
        River& river = rivers[r];
        const size_t count = river.points.size();
        if (count < 3) {
            continue;
        }
        vector<u8> flag(count, 0);
        vector<f32> excess(count, 0.0f);
        for (size_t i = 0; i < count; ++i) {
            const RiverPoint& pt = river.points[i];
            const f32 g = fineAt(pt.x, pt.z);
            excess[i] = g - pt.surface;
            flag[i] = excess[i] > kSaddleBuried &&
                              !underAcceptedLake(pt.x, pt.z)
                          ? 1
                          : 0;
        }
        size_t i = 0;
        while (i < count) {
            if (!flag[i]) {
                ++i;
                continue;
            }
            size_t j = i;
            f32 worst = excess[i];
            f32 runLen = 0.0f;
            while (j + 1 < count && flag[j + 1]) {
                runLen += std::hypot(
                    river.points[j + 1].x - river.points[j].x,
                    river.points[j + 1].z - river.points[j].z);
                ++j;
                worst = glm::max(worst, excess[j]);
            }
            if (runLen <= kSaddleMaxLen &&
                worst <= kSaddleMaxExcess) {
                const size_t a = i > 0 ? i - 1 : i;
                const size_t b =
                    j + 1 < count ? j + 1 : j;
                for (size_t k = a; k <= b; ++k) {
                    notchAt(river.points[k]);
                }
                ++stats.saddleRuns;
            }
            i = j + 1;
        }
    }
}

// The pass order IS the contract: each pass reads what the previous
// ones left (see the per-pass comments for who depends on whom). The
// clock name of a group is the historical phase name the logs carry.
struct RelevelPass {
    const char* name;
    void (*run)(HydroReleveling&);
};

const RelevelPass kRelevelPasses[] = {
    { "recenter", [](HydroReleveling& c) { c.recenterCourses(); } },
    { "retrace", [](HydroReleveling& c) { c.retraceBuriedRuns(); } },
    { "relakes", [](HydroReleveling& c) { c.reseatWindowLakes(); } },
    { "fill103",
      [](HydroReleveling& c) {
          c.buildEnvelopes();
          c.linkTributaries();
          c.fillProfiles();
      } },
    { "pools", [](HydroReleveling& c) { c.discoverPools(); } },
    { "cascade", [](HydroReleveling& c) { c.coalesceCascades(); } },
    { "slope", [](HydroReleveling& c) { c.capSlopes(); } },
    { "smooth", [](HydroReleveling& c) { c.smoothProfiles(); } },
    { "narrow", [](HydroReleveling& c) { c.narrowSteep(); } },
    { "clamps",
      [](HydroReleveling& c) {
          c.reclampTributaries();
          c.softenMapSeams();
          c.reseatPonds();
          c.dropRapidsFords();
      } },
    { "saddle", [](HydroReleveling& c) { c.cutSaddles(); } },
};

} // namespace

HydroRelevelStats relevelMapHydrology(
    const TileBakeParams& params, const HydrologyParams& hydrology,
    const MapGroundSampler& ground, const vector<f32>& preGround,
    MapHydrology& out, const std::function<void(const char*)>& lap) {
    HydroReleveling ctx { params, hydrology, ground, preGround, out };
    for (const RelevelPass& pass : kRelevelPasses) {
        pass.run(ctx);
        lap(pass.name);
    }
    return ctx.stats;
}

} // namespace render::terraingen
