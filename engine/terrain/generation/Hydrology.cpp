#include "engine/terrain/generation/Hydrology.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include <glm/glm.hpp>

#include "engine/core/Bezier.hpp"
#include "engine/core/Hash.hpp"
#include "engine/terrain/Noise.hpp"
#include "engine/terrain/generation/FluvialErosion.hpp"
#include "engine/terrain/generation/MasterNetwork.hpp"

namespace render::terraingen {

namespace {

// Grid-traced courses are runs of 8 m steps in the 8 grid directions —
// straight lines with hard 45° corners. Simplify (drop the stair-steps),
// smooth through the remaining corners (core::smoothPolyline — the
// carve pass then follows the same curve as the surface), re-derive the
// widths by arc length, and clamp the surface monotone downstream (the
// interpolating spline may overshoot on y).
void smoothRiver(River& river, f32 texel) {
    if (river.points.size() < 3) {
        return;
    }
    vector<Vec3> raw;
    raw.reserve(river.points.size());
    vector<f32> rawArc;
    vector<f32> rawWidth;
    f32 arc = 0.0f;
    for (size_t i = 0; i < river.points.size(); ++i) {
        const RiverPoint& pt = river.points[i];
        if (i > 0) {
            arc += std::hypot(pt.x - river.points[i - 1].x,
                              pt.z - river.points[i - 1].z);
        }
        raw.push_back({ pt.x, pt.surface, pt.z });
        rawArc.push_back(arc);
        rawWidth.push_back(pt.halfWidth);
    }
    vector<Vec3> simplified = core::simplifyPolylineXz(raw, texel * 0.9f);
    // Relaxation: the epsilon drainage wanders in tight sub-width
    // switchbacks on flat floors — a few Laplacian passes iron those out
    // while the large meanders (way above the window scale) survive.
    for (i32 pass = 0; pass < 3; ++pass) {
        vector<Vec3> next = simplified;
        for (size_t i = 1; i + 1 < simplified.size(); ++i) {
            next[i] = glm::mix(
                simplified[i],
                (simplified[i - 1] + simplified[i + 1]) * 0.5f, 0.55f);
        }
        simplified = std::move(next);
    }
    const vector<Vec3> smooth =
        core::smoothPolyline(simplified, texel * 0.5f);
    if (smooth.size() < 2) {
        return;
    }
    // Resample the smooth course at a regular step (a straight river
    // simplifies to two points — it still needs dense samples so the
    // ORIGINAL surface profile survives below). The y of the spline is
    // discarded: surface and width are re-read from the raw polyline by
    // arc-length fraction, then the surface is clamped monotone.
    vector<Vec3> course;
    course.push_back(smooth.front());
    const f32 stepLen = texel * 0.5f;
    for (size_t i = 1; i < smooth.size(); ++i) {
        const Vec3& a = smooth[i - 1];
        const Vec3& b = smooth[i];
        const f32 len = std::hypot(b.x - a.x, b.z - a.z);
        const u32 divs =
            glm::max(1u, static_cast<u32>(std::ceil(len / stepLen)));
        for (u32 s = 1; s <= divs; ++s) {
            const f32 t = static_cast<f32>(s) / static_cast<f32>(divs);
            course.push_back(glm::mix(a, b, t));
        }
    }
    const f32 rawTotal = glm::max(rawArc.back(), 1.0e-3f);
    f32 courseTotal = 0.0f;
    for (size_t i = 1; i < course.size(); ++i) {
        courseTotal += std::hypot(course[i].x - course[i - 1].x,
                                  course[i].z - course[i - 1].z);
    }
    courseTotal = glm::max(courseTotal, 1.0e-3f);
    // Width character (rivers should not all look alike): a per-river
    // factor hashed from the head position plus a slow modulation along
    // the course — deterministic, so the cache stays reproducible.
    const u32 riverSeed = core::hashU32(
        static_cast<u32>(static_cast<i32>(raw.front().x)) * 73856093u ^
        static_cast<u32>(static_cast<i32>(raw.front().z)) * 19349663u);
    const f32 riverFactor =
        0.7f + 0.6f * (static_cast<f32>(riverSeed) *
                       (1.0f / 4294967295.0f));
    river.points.clear();
    river.points.reserve(course.size());
    f32 outArc = 0.0f;
    f32 level = raw.front().y;
    size_t seg = 0;
    for (size_t i = 0; i < course.size(); ++i) {
        if (i > 0) {
            outArc += std::hypot(course[i].x - course[i - 1].x,
                                 course[i].z - course[i - 1].z);
        }
        const f32 rawAt = outArc / courseTotal * rawTotal;
        while (seg + 1 < rawArc.size() && rawArc[seg + 1] < rawAt) {
            ++seg;
        }
        const size_t next = glm::min(seg + 1, rawArc.size() - 1);
        const f32 span = glm::max(rawArc[next] - rawArc[seg], 1.0e-3f);
        const f32 t = glm::clamp((rawAt - rawArc[seg]) / span, 0.0f, 1.0f);
        level = glm::min(level, glm::mix(raw[seg].y, raw[next].y, t));
        RiverPoint pt;
        pt.x = course[i].x;
        pt.z = course[i].z;
        pt.surface = level;
        const f32 alongMod =
            0.85f + 0.3f * noise::value(riverSeed ^ 0x9e3779b9u,
                                        outArc * 0.011f, 0.0f);
        // Spring taper: a channel head EMERGES — hairline for the first
        // meters, full torrent width after ~45 m.
        const f32 spring = glm::max(
            noise::smoothstep01(0.0f, 45.0f, outArc), 0.10f);
        pt.halfWidth = glm::mix(rawWidth[seg], rawWidth[next], t) *
                       riverFactor * alongMod * spring;
        river.points.push_back(pt);
    }
}

// A pond disc at a trouble spot: level just under the local surface,
// mask = filled disc; the finalize pass digs the basin (Lake::dug).
Lake makePond(f32 x, f32 z, f32 surface, f32 radius, f32 texel) {
    Lake pond;
    pond.dug = 1;
    pond.level = surface - 0.1f;
    pond.minX = x - radius;
    pond.maxX = x + radius;
    pond.minZ = z - radius;
    pond.maxZ = z + radius;
    pond.maskTexel = texel;
    pond.maskWidth =
        static_cast<u32>(std::lround(2.0f * radius / texel)) + 1;
    pond.maskHeight = pond.maskWidth;
    pond.mask.assign(static_cast<size_t>(pond.maskWidth) *
                         pond.maskHeight,
                     0);
    for (u32 row = 0; row < pond.maskHeight; ++row) {
        for (u32 col = 0; col < pond.maskWidth; ++col) {
            const f32 px = pond.minX + static_cast<f32>(col) * texel;
            const f32 pz = pond.minZ + static_cast<f32>(row) * texel;
            if ((px - x) * (px - x) + (pz - z) * (pz - z) <=
                radius * radius) {
                pond.mask[static_cast<size_t>(row) * pond.maskWidth +
                          col] = 1;
                ++pond.cells;
            }
        }
    }
    return pond;
}

struct PondSpot {
    f32 x;
    f32 z;
    f32 surface;
    f32 radius;
};

// Proximity merge: a course that drifts within ribbon reach of an
// EARLIER course is truncated there and snapped onto it; the seam gets a
// pond spot. Run once on the raw traces AND once on the smoothed set —
// smoothing moves the courses, creating overlaps the raw pass never saw.
void mergeByProximity(vector<River>& rivers, f32 reachFactor,
                      vector<PondSpot>& spots) {
    struct HashedPoint {
        f32 x;
        f32 z;
        f32 halfWidth;
        f32 surface;
    };
    std::unordered_map<u64, vector<HashedPoint>> hash;
    constexpr f32 kHashCell = 24.0f;
    const auto hashKey = [](i32 hx, i32 hz) {
        return (static_cast<u64>(static_cast<u32>(hx)) << 32) |
               static_cast<u64>(static_cast<u32>(hz));
    };
    for (River& river : rivers) {
        size_t cutAt = river.points.size();
        for (size_t p = 0;
             p < river.points.size() && cutAt == river.points.size();
             ++p) {
            const RiverPoint& pt = river.points[p];
            const i32 hx = static_cast<i32>(std::floor(pt.x / kHashCell));
            const i32 hz = static_cast<i32>(std::floor(pt.z / kHashCell));
            for (i32 dz = -1; dz <= 1 && cutAt == river.points.size();
                 ++dz) {
                for (i32 dx = -1;
                     dx <= 1 && cutAt == river.points.size(); ++dx) {
                    const auto it = hash.find(hashKey(hx + dx, hz + dz));
                    if (it == hash.end()) {
                        continue;
                    }
                    for (const HashedPoint& other : it->second) {
                        const f32 reach = (pt.halfWidth +
                                           other.halfWidth) *
                                          reachFactor;
                        if (std::hypot(pt.x - other.x, pt.z - other.z) <
                            reach) {
                            cutAt = p;
                            RiverPoint snap = pt;
                            snap.x = other.x;
                            snap.z = other.z;
                            snap.surface =
                                glm::min(pt.surface, other.surface);
                            river.points.resize(p + 1);
                            river.points.push_back(snap);
                            // Pool-sized, never puddle-sized (see the
                            // hairpin spot below).
                            spots.push_back(
                                { snap.x, snap.z, snap.surface,
                                  glm::max((snap.halfWidth +
                                            other.halfWidth) *
                                               2.2f,
                                           15.0f) });
                            break;
                        }
                    }
                }
            }
        }
        for (const RiverPoint& pt : river.points) {
            const i32 hx = static_cast<i32>(std::floor(pt.x / kHashCell));
            const i32 hz = static_cast<i32>(std::floor(pt.z / kHashCell));
            hash[hashKey(hx, hz)].push_back(
                { pt.x, pt.z, pt.halfWidth, pt.surface });
        }
    }
}

} // namespace

vector<Lake> extractLakes(const GridSpec& spec, const vector<f32>& height,
                          const vector<f32>& filled,
                          const HydrologyParams& params,
                          vector<f32>* lakeDepthOut) {
    const size_t cells = spec.cells();
    // Lakes: connected components of filled-above-ground, flood-fill
    // labelled. The component's lowest routing height is the spill level
    // (the epsilon ramp only rises from there).
    vector<f32> lakeDepth(cells, 0.0f);
    for (size_t i = 0; i < cells; ++i) {
        const f32 depth = filled[i] - height[i];
        if (depth > params.minLakeDepth && height[i] > params.seaLevel) {
            lakeDepth[i] = depth;
        }
    }
    vector<Lake> lakes;
    vector<u32> label(cells, 0);
    const i32 n = static_cast<i32>(spec.n);
    u32 nextLabel = 0;
    vector<u32> stack;
    vector<u32> component;
    for (size_t seed = 0; seed < cells; ++seed) {
        if (lakeDepth[seed] <= 0.0f || label[seed] != 0) {
            continue;
        }
        ++nextLabel;
        Lake lake;
        lake.level = filled[seed];
        lake.minX = lake.maxX = spec.x(static_cast<u32>(seed % spec.n));
        lake.minZ = lake.maxZ = spec.z(static_cast<u32>(seed / spec.n));
        stack.assign(1, static_cast<u32>(seed));
        component.clear();
        label[seed] = nextLabel;
        while (!stack.empty()) {
            const u32 i = stack.back();
            stack.pop_back();
            component.push_back(i);
            ++lake.cells;
            const i32 cx = static_cast<i32>(i % spec.n);
            const i32 cz = static_cast<i32>(i / spec.n);
            const f32 wx = spec.x(static_cast<u32>(cx));
            const f32 wz = spec.z(static_cast<u32>(cz));
            lake.level = glm::min(lake.level, filled[i]);
            lake.minX = glm::min(lake.minX, wx);
            lake.maxX = glm::max(lake.maxX, wx);
            lake.minZ = glm::min(lake.minZ, wz);
            lake.maxZ = glm::max(lake.maxZ, wz);
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    const i32 px = cx + dx;
                    const i32 pz = cz + dz;
                    if (px < 0 || pz < 0 || px >= n || pz >= n) {
                        continue;
                    }
                    const size_t p = static_cast<size_t>(pz) * spec.n +
                                     static_cast<size_t>(px);
                    if (lakeDepth[p] > 0.0f && label[p] == 0) {
                        label[p] = nextLabel;
                        stack.push_back(static_cast<u32>(p));
                    }
                }
            }
        }
        if (lake.cells >= params.minLakeCells) {
            // Bbox-local basin mask (the render/query footprint).
            lake.maskTexel = spec.texelSize;
            lake.maskWidth =
                static_cast<u32>(std::lround((lake.maxX - lake.minX) /
                                             spec.texelSize)) +
                1;
            lake.maskHeight =
                static_cast<u32>(std::lround((lake.maxZ - lake.minZ) /
                                             spec.texelSize)) +
                1;
            lake.mask.assign(static_cast<size_t>(lake.maskWidth) *
                                 lake.maskHeight,
                             0);
            for (const u32 i : component) {
                const f32 wx = spec.x(i % spec.n);
                const f32 wz = spec.z(i / spec.n);
                const u32 mx = static_cast<u32>(std::lround(
                    (wx - lake.minX) / spec.texelSize));
                const u32 mz = static_cast<u32>(std::lround(
                    (wz - lake.minZ) / spec.texelSize));
                lake.mask[static_cast<size_t>(mz) * lake.maskWidth + mx] =
                    1;
            }
            lakes.push_back(std::move(lake));
        }
    }
    if (lakeDepthOut) {
        *lakeDepthOut = std::move(lakeDepth);
    }
    return lakes;
}

HydrologyResult extractHydrology(const GridSpec& spec,
                                 const vector<f32>& height,
                                 const HydrologyParams& params) {
    const size_t cells = spec.cells();
    HydrologyResult out;
    out.filled = priorityFloodFill(spec, height, params.seaLevel,
                                   params.minSlope);
    const FlowRouting flow =
        routeFlow(spec, out.filled, height, params.seaLevel);
    out.area = flow.area;
    out.lakes =
        extractLakes(spec, height, out.filled, params, &out.lakeDepth);
    const i32 n = static_cast<i32>(spec.n);

    // Rivers: channel cells (area over threshold, on land, not lake),
    // traced downstream from channel heads. A trace ends at the sea, at a
    // lake, at base level, or when it merges into an already-traced
    // channel (the junction point is appended so ribbons connect).
    vector<u8> channel(cells, 0);
    // Who claimed each channel cell (river index + 1): the junction
    // tests must ignore the CURRENT trace's own cells (the predecessor
    // is always an adjacent claimed cell).
    vector<u32> claimedBy(cells, 0);
    for (size_t i = 0; i < cells; ++i) {
        channel[i] = out.area[i] >= params.riverArea &&
                             height[i] > params.seaLevel &&
                             out.lakeDepth[i] <= 0.0f
                         ? 1
                         : 0;
    }
    // Channel heads: channel cells with no channel donor.
    vector<u8> hasChannelDonor(cells, 0);
    for (size_t i = 0; i < cells; ++i) {
        if (channel[i] && flow.receiver[i] != i) {
            hasChannelDonor[flow.receiver[i]] |= channel[i];
        }
    }
    const auto pointAt = [&](u32 i) {
        RiverPoint pt;
        const u32 cx = i % spec.n;
        const u32 cz = i / spec.n;
        pt.x = spec.x(cx);
        pt.z = spec.z(cz);
        pt.surface = out.filled[i];
        // Width: sub-sqrt growth of the drainage area, NARROWED by the
        // local slope — a steep torrent carries the same water fast and
        // stays a torrent; only the flats spread into wide rivers.
        const f32 base =
            params.widthCoef *
            std::pow(glm::max(out.area[i], 0.0f), params.widthExponent);
        const u32 xm = cx > 0 ? cx - 1 : 0;
        const u32 xp = glm::min(cx + 1, spec.n - 1);
        const u32 zm = cz > 0 ? cz - 1 : 0;
        const u32 zp = glm::min(cz + 1, spec.n - 1);
        const f32 gx = (height[static_cast<size_t>(cz) * spec.n + xp] -
                        height[static_cast<size_t>(cz) * spec.n + xm]) /
                       (static_cast<f32>(xp - xm) * spec.texelSize);
        const f32 gz = (height[static_cast<size_t>(zp) * spec.n + cx] -
                        height[static_cast<size_t>(zm) * spec.n + cx]) /
                       (static_cast<f32>(zp - zm) * spec.texelSize);
        const f32 slope = std::sqrt(gx * gx + gz * gz);
        const f32 narrow = glm::mix(
            1.0f, 0.45f,
            glm::clamp((slope - 0.05f) / (0.28f - 0.05f), 0.0f, 1.0f));
        pt.halfWidth = glm::max(base * narrow, 1.2f);
        return pt;
    };
    vector<PondSpot> spots;
    const auto junctionPond = [&](const RiverPoint& at, f32 other) {
        // Pool-sized, never puddle-sized (see the hairpin spot below).
        spots.push_back({ at.x, at.z, at.surface,
                          glm::max((at.halfWidth + other) * 2.2f,
                                   15.0f) });
    };
    // Phase 1 — raw traces. A junction fires when the current cell OR
    // any 8-neighbour is already claimed: two courses one cell apart
    // (epsilon drainage follows the grid on flat floors) MERGE instead
    // of running side by side forever.
    vector<River> raw;
    for (size_t head = 0; head < cells; ++head) {
        if (!channel[head] || hasChannelDonor[head]) {
            continue;
        }
        River river;
        const u32 myId = static_cast<u32>(raw.size()) + 1;
        u32 i = static_cast<u32>(head);
        while (true) {
            river.points.push_back(pointAt(i));
            if (claimedBy[i] != 0 && claimedBy[i] != myId) {
                junctionPond(river.points.back(),
                             river.points.back().halfWidth);
                break;
            }
            claimedBy[i] = myId;
            // Own cells only (the shared junction cell would leak the
            // RECEIVING river's drainage into a tributary's tier).
            river.mouthArea = glm::max(river.mouthArea, out.area[i]);
            const i32 cx = static_cast<i32>(i % spec.n);
            const i32 cz = static_cast<i32>(i / spec.n);
            u32 neighbourJunction = 0;
            bool hasNeighbour = false;
            for (i32 dz = -1; dz <= 1 && !hasNeighbour; ++dz) {
                for (i32 dx = -1; dx <= 1 && !hasNeighbour; ++dx) {
                    if (dx == 0 && dz == 0) {
                        continue;
                    }
                    const i32 px = cx + dx;
                    const i32 pz = cz + dz;
                    if (px < 0 || pz < 0 || px >= n || pz >= n) {
                        continue;
                    }
                    const size_t p = static_cast<size_t>(pz) * spec.n +
                                     static_cast<size_t>(px);
                    if (claimedBy[p] != 0 && claimedBy[p] != myId &&
                        channel[p] &&
                        flow.receiver[i] != static_cast<u32>(p)) {
                        neighbourJunction = static_cast<u32>(p);
                        hasNeighbour = true;
                    }
                }
            }
            if (hasNeighbour) {
                river.points.push_back(pointAt(neighbourJunction));
                junctionPond(river.points.back(),
                             river.points.back().halfWidth);
                break;
            }
            const u32 r = flow.receiver[i];
            if (r == i || height[r] <= params.seaLevel ||
                out.lakeDepth[r] > 0.0f) {
                if (r != i) {
                    river.points.push_back(pointAt(r)); // reach the shore
                }
                break;
            }
            i = r;
        }
        if (river.points.size() >= params.minRiverPoints) {
            raw.push_back(std::move(river));
        }
    }
    // Phase 2 — proximity merge on the RAW traces.
    mergeByProximity(raw, 1.25f, spots);
    // Phase 3 — smooth every survivor.
    vector<River> smoothed;
    for (River& river : raw) {
        if (river.points.size() < params.minRiverPoints) {
            continue;
        }
        smoothRiver(river, spec.texelSize);
        smoothed.push_back(std::move(river));
    }
    // Phase 4 — GLOBAL merge on the final smoothed set: smoothing MOVES
    // the courses, creating tangencies/crossings the raw pass never saw
    // (complex junctions used to slip through here, ribbons stabbing
    // each other with no pond and no tail dissolve).
    mergeByProximity(smoothed, 1.1f, spots);
    // Phase 5 — hairpin ponds on what remains.
    for (River& river : smoothed) {
        if (river.points.size() < params.minRiverPoints) {
            continue;
        }
        for (size_t p = 2; p + 2 < river.points.size(); p += 2) {
            const auto dir = [&](size_t a, size_t b) {
                const f32 dx = river.points[b].x - river.points[a].x;
                const f32 dz = river.points[b].z - river.points[a].z;
                const f32 len = std::hypot(dx, dz);
                return len > 1.0e-3f ? Vec2 { dx / len, dz / len }
                                     : Vec2 { 1.0f, 0.0f };
            };
            const Vec2 in = dir(p - 2, p);
            const Vec2 outDir = dir(p, p + 2);
            const f32 turn = std::acos(
                glm::clamp(in.x * outDir.x + in.y * outDir.y, -1.0f,
                           1.0f));
            if (turn > params.hairpinTurn) {
                const RiverPoint& at = river.points[p];
                // Pool-sized, never puddle-sized: a junction pond under
                // ~15 m radius reads as a puddle in the landscape (the
                // few-meter basins the landscape review flagged) — the
                // fix spot must look like a natural river pool.
                spots.push_back({ at.x, at.z, at.surface,
                                  glm::max(at.halfWidth * 3.0f, 15.0f) });
            }
        }
        out.rivers.push_back(std::move(river));
    }
    // Ponds: dedup close spots, dig-flagged lakes; tributary TAILS inside
    // a pond are trimmed (their ribbon dies under the flat surface).
    vector<Lake> ponds;
    for (const PondSpot& spot : spots) {
        bool merged = false;
        for (const Lake& existing : ponds) {
            const f32 cx = (existing.minX + existing.maxX) * 0.5f;
            const f32 cz = (existing.minZ + existing.maxZ) * 0.5f;
            const f32 r = (existing.maxX - existing.minX) * 0.5f;
            if (std::hypot(spot.x - cx, spot.z - cz) < (r + spot.radius)) {
                merged = true;
                break;
            }
        }
        if (!merged) {
            ponds.push_back(makePond(spot.x, spot.z, spot.surface,
                                     spot.radius, spec.texelSize));
        }
    }
    for (River& river : out.rivers) {
        while (river.points.size() > 2) {
            const RiverPoint& tail = river.points.back();
            bool inPond = false;
            for (const Lake& pond : ponds) {
                const f32 cx = (pond.minX + pond.maxX) * 0.5f;
                const f32 cz = (pond.minZ + pond.maxZ) * 0.5f;
                const f32 r = (pond.maxX - pond.minX) * 0.5f;
                if (std::hypot(tail.x - cx, tail.z - cz) < r * 0.7f) {
                    inPond = true;
                    break;
                }
            }
            if (!inPond) {
                break;
            }
            river.points.pop_back();
        }
    }
    out.lakes.insert(out.lakes.end(),
                     std::make_move_iterator(ponds.begin()),
                     std::make_move_iterator(ponds.end()));
    return out;
}

void classifyRivers(vector<River>& rivers, const HydrologyParams& params,
                    u32 seed, const vector<MasterRiver>& master) {
    for (River& river : rivers) {
        if (river.points.size() < 2) {
            continue;
        }
        river.tier = river.mouthArea >= params.riviereArea ? 1 : 0;
        // Fleuve promotion: a course matching a MASTER river inherits
        // the obstacle tier and a width floor from the TRUE drainage
        // area (the tile window truncates areas — a fleuve cannot be
        // known locally, only recognized). Match = most points within
        // two master texels of the master polyline.
        u32 matched = 0;
        constexpr f32 kMatchDist = 260.0f;
        vector<f32> areaAt(river.points.size(), 0.0f);
        for (size_t p = 0; p < river.points.size(); ++p) {
            const RiverPoint& pt = river.points[p];
            f32 best = kMatchDist * kMatchDist;
            for (const MasterRiver& mr : master) {
                for (const MasterNode& node : mr.nodes) {
                    const f32 dx = node.x - pt.x;
                    const f32 dz = node.z - pt.z;
                    const f32 d = dx * dx + dz * dz;
                    if (d < best) {
                        best = d;
                        areaAt[p] = node.area;
                    }
                }
            }
            matched += areaAt[p] > 0.0f ? 1 : 0;
        }
        if (matched * 2 >= river.points.size()) {
            river.tier = 2;
            // Width floor from the TRUE area at each point — WORLD-
            // stable (the master's areas grow downstream wherever the
            // tile window truncates the local ones, so neighbours
            // agree), bounded to the design band (~24-36 m channels)
            // and MONOTONE via the running max: an obstacle never
            // pinches back into a crossable brook.
            f32 runningHalf = 0.0f;
            for (size_t p = 0; p < river.points.size(); ++p) {
                if (areaAt[p] > 0.0f) {
                    const f32 floorHalf = glm::clamp(
                        params.widthCoef *
                            std::pow(areaAt[p], params.widthExponent),
                        12.0f, 18.0f);
                    runningHalf = glm::max(runningHalf, floorHalf);
                }
                river.points[p].halfWidth =
                    glm::max(river.points[p].halfWidth, runningHalf);
            }
        }
        // Fords on the rivières only: candidates on a jittered WORLD
        // grid (tile-independent by construction — the guarantee is
        // the grid's, like the landmark layers), adopted where the
        // course passes within reach. Ruisseaux need none (wadeable),
        // fleuves get none (the obstacle: crossings are bridge sites).
        river.fords.clear();
        if (river.tier != 1) {
            continue;
        }
        f32 minX = 1.0e30f;
        f32 minZ = 1.0e30f;
        f32 maxX = -1.0e30f;
        f32 maxZ = -1.0e30f;
        for (const RiverPoint& pt : river.points) {
            minX = glm::min(minX, pt.x);
            maxX = glm::max(maxX, pt.x);
            minZ = glm::min(minZ, pt.z);
            maxZ = glm::max(maxZ, pt.z);
        }
        const f32 cell = params.fordSpacing;
        const i32 gx0 = static_cast<i32>(
            std::floor((minX - params.fordReach) / cell));
        const i32 gx1 = static_cast<i32>(
            std::floor((maxX + params.fordReach) / cell));
        const i32 gz0 = static_cast<i32>(
            std::floor((minZ - params.fordReach) / cell));
        const i32 gz1 = static_cast<i32>(
            std::floor((maxZ + params.fordReach) / cell));
        for (i32 gz = gz0; gz <= gz1; ++gz) {
            for (i32 gx = gx0; gx <= gx1; ++gx) {
                const u32 h = core::hashU32(
                    (seed ^ 0x5f3d92c1u) ^
                    static_cast<u32>(gx) * 0x9e3779b9u ^
                    static_cast<u32>(gz) * 0x85ebca6bu);
                const f32 jx = (static_cast<f32>(h & 0xffffu) *
                                    (1.0f / 65535.0f) -
                                0.5f) *
                               0.7f;
                const f32 jz = (static_cast<f32>((h >> 16) & 0xffffu) *
                                    (1.0f / 65535.0f) -
                                0.5f) *
                               0.7f;
                const f32 cx =
                    (static_cast<f32>(gx) + 0.5f + jx) * cell;
                const f32 cz =
                    (static_cast<f32>(gz) + 0.5f + jz) * cell;
                f32 best = params.fordReach * params.fordReach;
                Vec2 spot { 0.0f, 0.0f };
                bool found = false;
                for (const RiverPoint& pt : river.points) {
                    const f32 dx = pt.x - cx;
                    const f32 dz = pt.z - cz;
                    const f32 d = dx * dx + dz * dz;
                    if (d < best) {
                        best = d;
                        spot = { pt.x, pt.z };
                        found = true;
                    }
                }
                if (found) {
                    river.fords.push_back(spot);
                }
            }
        }
    }
}

} // namespace render::terraingen
