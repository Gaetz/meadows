#include "engine/render/landscape/TreeGenerator.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/Hash.hpp"
#include "engine/render/MeshBuilder.hpp"

// Runions et al. 2007, "Modeling Trees with a Space Colonization
// Algorithm", with SDF-shaded billboard-card foliage (metaball SDF at
// branch tips, order-weighted radii, card normals from the SDF
// gradient). Journal: docs/RENDERING.md, brique 27b.
//
// Pipeline: crown envelope → attraction points → iterative colonization
// (each attractor pulls its CLOSEST node; nodes grow segments of length D
// toward the mean pull; attractors die inside the kill distance) →
// pipe-model radii (r^n = Σ children r^n, basipetal) → chain decimation →
// tapered tubes for the wood → metaball SDF from the tips → cross-plane
// clusters scattered in the SDF shell, normals = ∇SDF.

namespace render {

namespace {

using core::hashU32;
using core::HashRng;

// Artistic knobs live in ColonizedTreeParams;
// only the truly structural constants stay here.
constexpr u32 kMaxIterations = 120;
constexpr f32 kTipRadius = 0.020f;     // r0 at every branch tip (m)

struct Node {
    Vec3 position;
    i32 parent;      // -1 for the root
    Vec3 direction;  // from parent (unit); up for the root
    u32 childCount { 0 };
    u8 order { 0 };  // branch order: +1 for lateral children (see below)
    f32 radius { kTipRadius };
};

// Polynomial smooth minimum (Inigo Quilez) — the metaball merge.
f32 smoothMin(f32 a, f32 b, f32 k) {
    const f32 h = glm::clamp(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
    return glm::mix(b, a, h) - k * h * (1.0f - h);
}

struct Metaball {
    Vec3 center;
    f32 radius;
};

f32 canopySdf(const vector<Metaball>& balls, const Vec3& p, f32 smoothK) {
    f32 d = 1e9f;
    for (const Metaball& ball : balls) {
        d = smoothMin(d, glm::length(p - ball.center) - ball.radius,
                      smoothK);
    }
    return d;
}

Vec3 canopySdfGradient(const vector<Metaball>& balls, const Vec3& p,
                       f32 smoothK) {
    constexpr f32 kEps = 0.15f; // wide taps: SMOOTH normals are the point
    const f32 dx = canopySdf(balls, p + Vec3 { kEps, 0.0f, 0.0f }, smoothK) -
                   canopySdf(balls, p - Vec3 { kEps, 0.0f, 0.0f }, smoothK);
    const f32 dy = canopySdf(balls, p + Vec3 { 0.0f, kEps, 0.0f }, smoothK) -
                   canopySdf(balls, p - Vec3 { 0.0f, kEps, 0.0f }, smoothK);
    const f32 dz = canopySdf(balls, p + Vec3 { 0.0f, 0.0f, kEps }, smoothK) -
                   canopySdf(balls, p - Vec3 { 0.0f, 0.0f, kEps }, smoothK);
    const Vec3 g { dx, dy, dz };
    const f32 len = glm::length(g);
    return len > 1e-6f ? g / len : Vec3 { 0.0f, 1.0f, 0.0f };
}

// One BILLBOARD leaf card (a single camera-facing card reads better
// than a mechanical fixed 60° cross). The mesh stores a
// DEGENERATE quad — four vertices at the cluster center — and encodes
// the corner in uv: uv.x = -10 + cornerX·halfSize (the -10 bias is the
// card FLAG, unambiguous against wood/lobe uv in [0,1]), uv.y =
// cornerY·halfSize. tree.vert expands toward the camera at render time
// (shadow_prop.vert toward the light), and the LIGHTING normal stays the
// SDF gradient — the whole point of the technique.
constexpr f32 kCardFlagBias = -10.0f;
// The atlas SLOT rides the bias in -20 steps (-10, -30, -50, ... for
// slots 0..7): tree.vert / shadow_prop.vert decode the slot and route
// the card to its tile of the leaf-mask atlas.
constexpr f32 kCardSlotStep = -20.0f;

void appendBillboardCard(MeshData& mesh, const Vec3& center, f32 halfSize,
                         const Vec3& normal, const Vec3& color,
                         i32 slot = 0) {
    const f32 bias =
        kCardFlagBias + kCardSlotStep * static_cast<f32>(glm::clamp(slot, 0, 7));
    const u32 base = static_cast<u32>(mesh.vertices.size());
    for (const Vec2 corner : { Vec2 { -1.0f, -1.0f }, Vec2 { 1.0f, -1.0f },
                               Vec2 { 1.0f, 1.0f }, Vec2 { -1.0f, 1.0f } }) {
        mesh.vertices.push_back(
            { center, normal,
              { bias + corner.x * halfSize,
                corner.y * halfSize },
              color });
    }
    // CCW after the camera-facing expansion (right×up points at the eye).
    const u32 quad[6] = { 0, 1, 2, 0, 2, 3 };
    for (u32 i : quad) {
        mesh.indices.push_back(base + i);
    }
}

// The seed-stable core every consumer shares — the visual mesh LODs and
// the shadow proxy must agree on silhouette. Grown from the SHAPE stream
// only, so it never depends on detail level or card scatter.
struct GrownTree {
    vector<Node> nodes;
    vector<Metaball> balls; // largest-first, capped (SDF cost)
    f32 trunkBase { 0.0f };
    f32 totalHeight { 0.0f };
};

GrownTree growColonizedTree(u32 seed, const ColonizedTreeParams& params) {
    HashRng shapeRng { hashU32(seed ^ 0x5c01a11eu) };

    // --- Crown envelope + attraction points ------------------------------
    const f32 trunkBase =
        params.trunkBaseMin +
        shapeRng.next() * (params.trunkBaseMax - params.trunkBaseMin);
    const f32 crownHeight =
        params.crownHeightMin +
        shapeRng.next() * (params.crownHeightMax - params.crownHeightMin);
    const f32 crownRadius =
        params.crownRadiusMin +
        shapeRng.next() * (params.crownRadiusMax - params.crownRadiusMin);
    const Vec3 crownCenter { (shapeRng.next() - 0.5f) * 0.5f,
                             trunkBase + crownHeight * 0.55f,
                             (shapeRng.next() - 0.5f) * 0.5f };
    const f32 totalHeight = trunkBase + crownHeight + 0.4f;

    const u32 attractorCount =
        static_cast<u32>(glm::clamp(params.attractorCount, 50, 2000));
    vector<Vec3> attractors;
    attractors.reserve(attractorCount);
    while (attractors.size() < attractorCount) {
        if (params.leaderBias > 0.0f &&
            shapeRng.next() < params.leaderBias) {
            // Apical leader: a thin axial column through the whole
            // crown — the trunk climbs straight through it.
            const f32 y01 = shapeRng.next();
            attractors.push_back(
                { crownCenter.x +
                      (shapeRng.next() - 0.5f) * 0.1f * crownRadius,
                  trunkBase + y01 * (crownHeight + 0.4f),
                  crownCenter.z +
                      (shapeRng.next() - 0.5f) * 0.1f * crownRadius });
            continue;
        }
        // Rejection-sample the ellipsoid (deterministic sequence).
        const Vec3 unit { shapeRng.next() * 2.0f - 1.0f,
                          shapeRng.next() * 2.0f - 1.0f,
                          shapeRng.next() * 2.0f - 1.0f };
        if (glm::dot(unit, unit) > 1.0f) {
            continue;
        }
        if (params.crownTaper > 0.0f) {
            // Cone profile: the allowed radius shrinks toward the apex.
            const f32 y01 = unit.y * 0.5f + 0.5f;
            const f32 allowed =
                glm::mix(1.0f, 1.0f - y01,
                         glm::clamp(params.crownTaper, 0.0f, 1.0f));
            if (unit.x * unit.x + unit.z * unit.z > allowed * allowed) {
                continue;
            }
        }
        attractors.push_back(crownCenter +
                             Vec3 { unit.x * crownRadius,
                                    unit.y * crownHeight * 0.5f,
                                    unit.z * crownRadius });
    }

    // --- Space colonization ----------------------------------------------
    vector<Node> nodes;
    nodes.reserve(512);
    nodes.push_back({ Vec3 { 0.0f }, -1, Vec3 { 0.0f, 1.0f, 0.0f } });

    vector<Vec3> pullSum;   // per node, this iteration
    vector<u32> pullCount;
    const Vec3 tropism { 0.0f, params.tropism, 0.0f }; // upward bias (eq. 3)
    for (u32 iteration = 0; iteration < kMaxIterations; ++iteration) {
        pullSum.assign(nodes.size(), Vec3 { 0.0f });
        pullCount.assign(nodes.size(), 0u);
        // Unlimited influence radius (paper fig. 5a: d_i = ∞ gives the
        // clearly delineated trunk we want) — each attractor pulls its
        // closest node, full stop.
        for (const Vec3& attractor : attractors) {
            f32 best = 1e9f;
            u32 bestNode = 0;
            for (u32 n = 0; n < nodes.size(); ++n) {
                const f32 d2 =
                    glm::dot(attractor - nodes[n].position,
                             attractor - nodes[n].position);
                if (d2 < best) {
                    best = d2;
                    bestNode = n;
                }
            }
            pullSum[bestNode] += glm::normalize(
                attractor - nodes[bestNode].position);
            pullCount[bestNode] += 1;
        }
        const u32 nodeCountBefore = static_cast<u32>(nodes.size());
        for (u32 n = 0; n < nodeCountBefore; ++n) {
            if (pullCount[n] == 0) {
                continue;
            }
            const f32 pullLen = glm::length(pullSum[n]);
            if (pullLen < 1e-4f) {
                continue; // opposing attractors cancel out (paper §2)
            }
            Vec3 direction =
                glm::normalize(pullSum[n] / pullLen + tropism);
            if (params.lateralFlatten > 0.0f) {
                // Whorl shelves: growth already heading sideways is
                // pressed toward the horizontal with a slight droop;
                // near-vertical growth (the leader) stays free.
                const f32 sideways = glm::clamp(
                    (1.0f - glm::abs(direction.y)) * 2.0f - 0.6f, 0.0f,
                    1.0f);
                const f32 press = glm::clamp(params.lateralFlatten, 0.0f,
                                             1.0f) *
                                  sideways;
                if (press > 0.0f) {
                    direction.y = glm::mix(direction.y, -0.08f, press);
                    direction = glm::normalize(direction);
                }
            }
            const Vec3 position =
                nodes[n].position + direction * params.segment;
            // Degenerate-growth guard: don't stack a node onto a sibling.
            bool duplicate = false;
            for (u32 m = nodeCountBefore; m < nodes.size(); ++m) {
                if (glm::dot(position - nodes[m].position,
                             position - nodes[m].position) <
                    (0.5f * params.segment) * (0.5f * params.segment)) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            nodes[n].childCount += 1;
            nodes.push_back({ position, static_cast<i32>(n), direction });
        }
        if (nodes.size() == nodeCountBefore) {
            break; // no growth: remaining attractors are unreachable
        }
        // Kill attractors reached by the NEW nodes.
        std::erase_if(attractors, [&](const Vec3& attractor) {
            for (u32 m = nodeCountBefore; m < nodes.size(); ++m) {
                if (glm::dot(attractor - nodes[m].position,
                             attractor - nodes[m].position) <
                    params.killDistance * params.killDistance) {
                    return true;
                }
            }
            return false;
        });
        if (attractors.size() < attractorCount / 20) {
            break; // crown filled
        }
    }

    // --- Branch order + pipe-model radii (children precede nothing:
    // creation order guarantees parent < child, so one reverse sweep). ---
    for (u32 n = 1; n < nodes.size(); ++n) {
        const Node& parent = nodes[static_cast<u32>(nodes[n].parent)];
        // Lateral children (diverging from the parent's own direction)
        // increment the order — the colleague's metaball weighting needs
        // it, or every twig tip carries the weight of a limb.
        const f32 alignment = glm::dot(nodes[n].direction, parent.direction);
        nodes[n].order = static_cast<u8>(glm::min<u32>(
            parent.order + (alignment < 0.85f ? 1u : 0u), 250u));
    }
    for (u32 n = static_cast<u32>(nodes.size()); n-- > 1;) {
        Node& parent = nodes[static_cast<u32>(nodes[n].parent)];
        parent.radius = std::pow(
            std::pow(parent.radius, params.pipeExponent) +
                std::pow(nodes[n].radius, params.pipeExponent),
            1.0f / params.pipeExponent);
    }
    for (Node& node : nodes) {
        node.radius = glm::min(node.radius, 0.30f);
    }

    // --- Metaballs at the tips, order-weighted ----------------------------
    vector<Metaball> balls;
    for (const Node& node : nodes) {
        if (node.childCount != 0 || node.position.y < trunkBase) {
            continue;
        }
        // The colleague's key detail: weight by branch order, or every
        // tip carries the same weight and the SDF collapses back into
        // one uniform blob.
        const f32 radius = glm::clamp(
            params.tipBallRadius *
                std::pow(params.tipOrderFalloff,
                         static_cast<f32>(node.order)),
            params.tipBallMin,
            glm::max(params.tipBallRadius, params.tipBallMin + 0.05f));
        balls.push_back({ node.position, radius });
    }
    if (balls.empty()) { // degenerate seed: keep the result valid
        balls.push_back({ crownCenter, crownRadius * 0.7f });
    }
    // Cap the SDF cost: keep the LARGEST balls (they define the volume).
    std::sort(balls.begin(), balls.end(),
              [](const Metaball& a, const Metaball& b) {
                  return a.radius > b.radius;
              });
    if (balls.size() > 64) {
        balls.resize(64);
    }

    return { std::move(nodes), std::move(balls), trunkBase, totalHeight };
}

// Wood: chain-decimated tapered tubes. `detail` picks the twig-culling
// radius floor (past the near level, sub-centimeter wood reads as noise
// and costs like geometry) and derives the look knobs: tubeSides applies
// at full detail (low twin one less, ultra always 3), curvePreserve
// tightens the decimation so the growth trajectory's bends survive
// (halved on the low twin, off on ultra), curveSubdiv rounds the elbows
// with Catmull-Rom points (same LOD ladder).
void appendWood(MeshData& mesh, const vector<Node>& nodes, u32 detail,
                const ColonizedTreeParams& params, u32 seed) {
    const Vec3 barkColor { 0.085f, 0.048f, 0.026f }; // generateTree's bark
    const f32 segment = params.segment;
    const i32 baseSides = glm::clamp(params.tubeSides, 3, 12);
    // detail 3 = the hero-near twin (camera chunk only): DOUBLE ring
    // sides + one more curve subdivision — build-time tessellation
    // (MoltenVK has no usable hardware tessellation).
    const u32 tubeSides =
        detail >= 3 ? static_cast<u32>(glm::min(baseSides * 2, 24))
        : detail == 2 ? static_cast<u32>(baseSides)
        : detail == 1 ? static_cast<u32>(glm::max(baseSides - 1, 3))
                      : 3u;
    const f32 preserve =
        glm::clamp(params.curvePreserve, 0.0f, 1.0f) *
        (detail >= 2 ? 1.0f : detail == 1 ? 0.5f : 0.0f);
    const i32 subdiv =
        detail >= 3   ? glm::clamp(params.curveSubdiv, 0, 3) + 1
        : detail == 2 ? glm::clamp(params.curveSubdiv, 0, 3)
        : detail == 1 ? glm::clamp(params.curveSubdiv, 0, 3) / 2
                      : 0;
    // Decimation collapses near-collinear single-child runs; preserve
    // raises the alignment bar and shortens the run cap, keeping the
    // real growth wiggle in the polyline.
    const f32 alignThreshold = glm::mix(0.95f, 0.9995f, preserve);
    const f32 runCap = glm::mix(1.1f, 0.3f, preserve);
    const f32 minRadius =
        detail >= 2 ? 0.012f : detail == 1 ? 0.025f : 0.045f;
    const f32 pathJitter = glm::clamp(params.pathJitter, 0.0f, 1.0f) *
                           (detail == 0 ? 0.0f : 1.0f);
    const f32 ringIrregularity =
        glm::clamp(params.ringIrregularity, 0.0f, 1.0f);
    const f32 sideMinFraction =
        glm::clamp(params.sideMinFraction, 0.25f, 1.0f);
    const f32 rootRadius = glm::max(nodes[0].radius, 1e-4f);

    // Deterministic per-POINT hash (quantized original position, mm):
    // every LOD keeps a subset of the same nodes, so the kinks agree.
    const auto positionHash = [](const Vec3& p) {
        u32 h = 0x9e3779b9u;
        h = hashU32(h ^ static_cast<u32>(
                        static_cast<i32>(std::lround(p.x * 1000.0f))));
        h = hashU32(h ^ static_cast<u32>(
                        static_cast<i32>(std::lround(p.y * 1000.0f))));
        h = hashU32(h ^ static_cast<u32>(
                        static_cast<i32>(std::lround(p.z * 1000.0f))));
        return h;
    };
    // Ring-count taper by HALVING: each time a chain's base radius drops
    // below half the previous threshold, the ring count halves (an even
    // tubeSides halves cleanly: 12 -> 6 -> 3, 8 -> 4) — face width stays
    // roughly constant since the perimeter halves too. The fraction knob
    // is the floor; at 1 the loop never runs (constant count).
    const auto sidesFor = [&](f32 radius) {
        const i32 floorSides = glm::max(
            3, static_cast<i32>(std::lround(static_cast<f32>(tubeSides) *
                                            sideMinFraction)));
        i32 sides = static_cast<i32>(tubeSides);
        f32 threshold = rootRadius * 0.5f;
        while (sides / 2 >= floorSides && radius < threshold) {
            sides /= 2;
            threshold *= 0.5f;
        }
        return static_cast<u32>(glm::max(sides, 3));
    };

    // Root flare (the SpeedTree "flares" idea): near the ground the
    // trunk widens into buttress lobes — a radial multiplier on the
    // ROOT chain's ring vertices, angular harmonics x squared height
    // falloff. Phases roll from the TREE seed, and the tube basis is
    // parallel-transported (no twist), so the lobes stay vertically
    // continuous and every LOD agrees.
    const f32 flareAmount = glm::clamp(params.flareAmount, 0.0f, 3.0f);
    const f32 flareHeight = glm::max(params.flareHeight, 0.01f);
    const f32 flareLobes =
        static_cast<f32>(glm::clamp(params.flareLobes, 1, 8));
    HashRng flareRng { hashU32(seed ^ 0xf1a2e001u) };
    const f32 flarePhase1 = flareRng.next() * 6.2831853f;
    const f32 flarePhase2 = flareRng.next() * 6.2831853f;
    const f32 flareBaseY = nodes[0].position.y;
    const auto flareMult = [&](const Vec3& center, const Vec3& dir) {
        const f32 h =
            glm::clamp((center.y - flareBaseY) / flareHeight, 0.0f, 1.0f);
        if (h >= 1.0f) {
            return 1.0f;
        }
        const f32 decay = (1.0f - h) * (1.0f - h);
        const f32 theta = std::atan2(dir.z, dir.x);
        const f32 profile =
            0.5f + 0.35f * std::sin(flareLobes * theta + flarePhase1) +
            0.15f * std::sin(flareLobes * 2.0f * theta + flarePhase2);
        return 1.0f + flareAmount * decay * profile;
    };

    struct PathPoint {
        Vec3 position;
        f32 radius;
    };
    vector<PathPoint> path;    // one chain, tip -> root order
    vector<PathPoint> refined; // after Catmull-Rom rounding
    vector<TubePoint> tube;    // root -> tip, welded-ring emission
    vector<TubePoint> dense;   // flare ring densification scratch

    // Walk each chain from a branching point (or tip) down to the previous
    // branching point, collecting the decimated polyline, then emit one
    // tapered tube per (refined) segment.
    for (u32 n = 1; n < nodes.size(); ++n) {
        const bool chainEnd =
            nodes[n].childCount != 1; // tip or branching point
        if (!chainEnd) {
            continue;
        }
        path.clear();
        path.push_back({ nodes[n].position, nodes[n].radius });
        // Ascend toward the root until the previous chain end.
        u32 cursor = n;
        while (cursor != 0) {
            // Collapse while the parent is a pass-through node whose
            // direction stays aligned and the run stays short.
            u32 runTop = cursor;
            f32 runLength = segment;
            while (nodes[runTop].parent > 0) {
                const u32 next = static_cast<u32>(nodes[runTop].parent);
                if (nodes[next].childCount != 1 ||
                    glm::dot(nodes[runTop].direction,
                             nodes[next].direction) < alignThreshold ||
                    runLength > runCap) {
                    break;
                }
                runTop = next;
                runLength += segment;
            }
            const u32 top =
                nodes[runTop].parent >= 0
                    ? static_cast<u32>(nodes[runTop].parent)
                    : 0u;
            path.push_back({ nodes[top].position, nodes[top].radius });
            cursor = top;
            if (nodes[cursor].childCount > 1 || cursor == 0) {
                break; // the parent chain is someone else's walk
            }
        }
        const bool flaredChain = cursor == 0 && flareAmount > 0.0f;

        // Trajectory kinks: displace the kept INTERIOR points (chain
        // ends anchor junctions and the foliage SDF, they never move).
        // Deterministic per original position, scaled by the growth
        // step — sharp breaks as-is, waves once subdivision rounds them.
        if (pathJitter > 0.0f && path.size() > 2) {
            for (size_t i = 1; i + 1 < path.size(); ++i) {
                HashRng jitterRng { positionHash(path[i].position) };
                const Vec3 offset { jitterRng.next() * 2.0f - 1.0f,
                                    jitterRng.next() * 2.0f - 1.0f,
                                    jitterRng.next() * 2.0f - 1.0f };
                path[i].position += offset * (pathJitter * 0.25f * segment);
            }
        }

        // Catmull-Rom rounding: interior samples per segment bend the
        // elbows; the original points stay put (junctions weld). Radii
        // lerp inside a segment.
        const vector<PathPoint>* emitted = &path;
        if (subdiv > 0 && path.size() >= 3) {
            refined.clear();
            refined.push_back(path[0]);
            const auto at = [&](i32 i) -> const Vec3& {
                return path[static_cast<size_t>(glm::clamp(
                                i, 0, static_cast<i32>(path.size()) - 1))]
                    .position;
            };
            for (i32 j = 0; j + 1 < static_cast<i32>(path.size()); ++j) {
                const Vec3& p0 = at(j - 1);
                const Vec3& p1 = at(j);
                const Vec3& p2 = at(j + 1);
                const Vec3& p3 = at(j + 2);
                for (i32 k = 1; k <= subdiv; ++k) {
                    const f32 t = static_cast<f32>(k) /
                                  static_cast<f32>(subdiv + 1);
                    const f32 t2 = t * t;
                    const f32 t3 = t2 * t;
                    const Vec3 position =
                        0.5f * ((2.0f * p1) + (-p0 + p2) * t +
                                (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) *
                                    t2 +
                                (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
                    const f32 radius =
                        glm::mix(path[static_cast<size_t>(j)].radius,
                                 path[static_cast<size_t>(j) + 1].radius,
                                 t);
                    refined.push_back({ position, radius });
                }
                refined.push_back(path[static_cast<size_t>(j) + 1]);
            }
            emitted = &refined;
        }
        // Twig cull: drop the tip-side points below the radius floor
        // (radii grow rootward along a chain — pipe model), then emit
        // the WHOLE chain as one welded tube: shared mitered rings close
        // every bend exactly. Ring count and angular jitter are
        // per-chain (welded rings must match vertex for vertex); the
        // jitter seed is the chain tip — stable across LODs.
        size_t start = 0;
        while (start < emitted->size() &&
               (*emitted)[start].radius < minRadius) {
            ++start;
        }
        if (emitted->size() - start < 2) {
            continue;
        }
        tube.clear();
        for (size_t i = emitted->size(); i > start; --i) {
            const PathPoint& point = (*emitted)[i - 1];
            if (!tube.empty() &&
                glm::distance(tube.back().position, point.position) <
                    1e-5f) {
                continue; // degenerate duplicate (jittered coincidence)
            }
            tube.push_back({ point.position, point.radius });
        }
        if (tube.size() >= 2) {
            // Decimation collapses the straight base into meter-long
            // runs — too coarse for the flare's height profile. Insert
            // lerped rings (~0.25 m spacing) below flareHeight; the
            // insertion is a pure function of positions, so LODs agree
            // (skipped on the 3-sided ultra twin, sub-texel there).
            if (flaredChain && detail >= 1) {
                dense.clear();
                for (size_t i = 0; i + 1 < tube.size(); ++i) {
                    const TubePoint& a = tube[i];
                    const TubePoint& b = tube[i + 1];
                    dense.push_back(a);
                    if (a.position.y - flareBaseY < flareHeight) {
                        const i32 cuts = static_cast<i32>(
                            glm::distance(a.position, b.position) / 0.25f);
                        for (i32 c = 1; c <= cuts; ++c) {
                            const f32 t = static_cast<f32>(c) /
                                          static_cast<f32>(cuts + 1);
                            dense.push_back(
                                { glm::mix(a.position, b.position, t),
                                  glm::mix(a.radius, b.radius, t) });
                        }
                    }
                }
                dense.push_back(tube.back());
            }
            const vector<TubePoint>& emitTube =
                flaredChain && detail >= 1 ? dense : tube;
            appendPolylineTube(mesh, emitTube,
                               sidesFor(tube.front().radius), barkColor,
                               ringIrregularity,
                               positionHash(path[0].position),
                               flaredChain ? TubeRadialFn(flareMult)
                                           : TubeRadialFn {});
        }
    }

    // Fork knuckles: sibling chains meet a branching node with rings in
    // different planes (and possibly different counts after halving) —
    // a small faceted knot at the node's radius fills the lens-shaped
    // openings and reads as the natural fork bulge. Skipped on the
    // ultra twin (3-sided distant wood, sub-texel at its range).
    if (detail >= 1) {
        for (u32 n = 1; n < nodes.size(); ++n) {
            if (nodes[n].childCount >= 2 && nodes[n].radius >= minRadius) {
                appendBlob(mesh, positionHash(nodes[n].position),
                           nodes[n].position, nodes[n].radius * 1.05f,
                           0.04f, barkColor, 0);
            }
        }
    }
}

} // namespace

MeshData generateColonizedTree(u32 seed, u32 detail,
                               const ColonizedTreeParams& params) {
    // The scatter stream is independent from the shape stream, so the
    // SKELETON never depends on `detail` — the three LODs of one seed
    // must share silhouette and colors.
    HashRng scatterRng { hashU32(seed ^ 0xf011a9e5u) };
    const GrownTree tree = growColonizedTree(seed, params);
    const vector<Metaball>& balls = tree.balls;
    const f32 totalHeight = tree.totalHeight;

    MeshData mesh;
    appendWood(mesh, tree.nodes, detail, params, seed);
    const u32 woodVertexCount = static_cast<u32>(mesh.vertices.size());

    // --- Foliage: billboard leaf cards in the SDF shell -------------------
    // ONE camera-facing card per clump, leaf-clump sized, in a shell
    // TIGHTENED against the surface — the silhouette is where cards pay,
    // and a sparse interior never shows.
    const u32 baseClusters = detail >= 2 ? 560u : detail == 1 ? 260u : 120u;
    const u32 clusterCount = glm::clamp(
        static_cast<u32>(static_cast<f32>(baseClusters) *
                         glm::clamp(params.foliageDensity, 0.1f, 8.0f)),
        1u, 4000u);
    // Spray candidates (conifer habit): the outer lateral branches —
    // cards ride them instead of the SDF shell. Built only when the
    // knob is on so the neutral scatter stream stays bit-exact.
    vector<u32> sprayNodes;
    if (params.sprayFoliage > 0.0f) {
        for (u32 n = 1; n < tree.nodes.size(); ++n) {
            if (tree.nodes[n].order >= 1 &&
                tree.nodes[n].position.y > tree.trunkBase * 0.8f) {
                sprayNodes.push_back(n);
            }
        }
    }
    u32 emitted = 0;
    for (u32 c = 0; c < clusterCount * 6u && emitted < clusterCount; ++c) {
        // The scatter stream runs the SAME sequence at every detail level
        // (clusterCount only truncates it): LODs agree on where the
        // canopy mass sits.
        Vec3 position;
        if (!sprayNodes.empty() &&
            scatterRng.next() <
                glm::clamp(params.sprayFoliage, 0.0f, 1.0f)) {
            // A card somewhere along an outer branch, slightly loose.
            const u32 pick = static_cast<u32>(
                                 scatterRng.next() *
                                 static_cast<f32>(sprayNodes.size())) %
                             static_cast<u32>(sprayNodes.size());
            const Node& node = tree.nodes[sprayNodes[pick]];
            const Node& parent =
                tree.nodes[static_cast<u32>(node.parent)];
            const f32 t = scatterRng.next();
            position = glm::mix(parent.position, node.position, t) +
                       Vec3 { scatterRng.spread(), scatterRng.spread(),
                              scatterRng.spread() } *
                           0.10f;
        } else {
            const Metaball& ball =
                balls[static_cast<u32>(scatterRng.next() *
                                       static_cast<f32>(balls.size())) %
                      balls.size()];
            position = ball.center;
            for (u32 attempt = 0; attempt < 24; ++attempt) {
                const Vec3 candidate =
                    ball.center +
                    Vec3 { (scatterRng.next() * 2.0f - 1.0f),
                           (scatterRng.next() * 2.0f - 1.0f),
                           (scatterRng.next() * 2.0f - 1.0f) } *
                        (ball.radius * 1.10f);
                const f32 d = canopySdf(balls, candidate, params.smoothK);
                if (d > -0.30f && d < -0.02f) {
                    position = candidate;
                    break;
                }
            }
        }

        const Vec3 normal = canopySdfGradient(balls, position, params.smoothK);
        // Light-seeking density gradient:
        // the card count is FIXED (the loop draws candidates until
        // clusterCount land — same cost), but acceptance follows the
        // canopy's outward direction: G^normal.y = xG where it faces
        // up, x1 on the sides, x1/G underneath. Leaves seek light;
        // undersides thin out.
        const f32 gradient = glm::max(params.densityGradient, 1.0f);
        const f32 weight = std::pow(gradient, normal.y); // 1/G .. G
        if (scatterRng.next() * gradient > weight) {
            continue;
        }

        const f32 hue = scatterRng.next();
        // Palette centered on the MEADOW color (#6FA160 linear —
        // grassAlbedo/kGrassAlbedo): canopy and grass read as one
        // family; the small hue roll keeps the crown alive.
        Vec3 leafColor =
            glm::mix(Vec3 { 0.135f, 0.340f, 0.115f },
                     Vec3 { 0.180f, 0.375f, 0.117f }, hue);
        // Sunlit-crown gradient, baked here (the uv loop below is
        // wood-only now — card uv carries the billboard corner).
        leafColor *= 1.0f + 0.25f * glm::clamp(position.y / totalHeight,
                                               0.0f, 1.0f);
        const f32 halfSize =
            params.cardHalfSizeMin +
            scatterRng.next() *
                (params.cardHalfSizeMax - params.cardHalfSizeMin);
        appendBillboardCard(mesh, position, halfSize, normal, leafColor,
                            params.leafStyle);
        ++emitted;
    }

    // --- Wind weights + vertical gradient — WOOD ONLY (card uv is the
    // billboard encoding; tree.vert gives cards a fixed sway weight). ----
    for (u32 i = 0; i < woodVertexCount; ++i) {
        MeshVertex& vertex = mesh.vertices[i];
        const f32 height01 =
            glm::clamp(vertex.position.y / totalHeight, 0.0f, 1.0f);
        // uv.y < -0.5 flags BARK (triplanar in tree.frag); nothing else
        // reads the lane for non-card wood.
        vertex.uv = { height01 * 0.30f, -1.0f };
    }
    return mesh;
}

MeshData generateColonizedTreeShadowProxy(u32 seed,
                                          const ColonizedTreeParams& params) {
    const GrownTree tree = growColonizedTree(seed, params);
    MeshData mesh;
    appendWood(mesh, tree.nodes, 0, params, seed);
    // The canopy metaballs ARE the shadow volume: one 20-face icosahedron
    // per ball (largest first — they define the mass) instead of the card
    // cloud. Opaque, so shadow_prop skips the leaf-mask cutout entirely.
    const Vec3 leafColor { 0.158f, 0.358f, 0.116f }; // meadow family
    const size_t ballCount = std::min<size_t>(tree.balls.size(), 16);
    for (size_t i = 0; i < ballCount; ++i) {
        appendBlob(mesh,
                   hashU32(seed ^ (0xb10bca57u + static_cast<u32>(i))),
                   tree.balls[i].center, tree.balls[i].radius,
                   0.10f, leafColor, 0);
    }
    return mesh;
}

vector<u8> generateLeafMaskPixels(u32 size, u32 seed,
                                  const ColonizedTreeParams& params,
                                  i32 shape) {
    // r = brightness (0 = x0.7, 255 = x1.3 in tree.frag), g/b unused,
    // a = coverage. Painter's order: a later leaf overwrites the shade
    // where it covers more than what's already there.
    vector<u8> pixels(static_cast<size_t>(size) * size * 4, 0);
    HashRng rng { hashU32(seed ^ 0x1eafca9du) };
    const f32 fsize = static_cast<f32>(size);
    const i32 leafCount = std::max(1, params.leafCount);
    for (i32 leaf = 0; leaf < leafCount; ++leaf) {
        const f32 length =
            params.leafSizeMin +
            rng.next() * (params.leafSizeMax - params.leafSizeMin);
        // Shape family: outline width ratio + edge profile (below).
        const f32 width =
            length * (shape == 1 ? 0.07f : 0.34f + rng.next() * 0.14f);
        // Radial placement, denser toward the center, capped so the whole
        // leaf stays inside the card (no straight clip at the border).
        const f32 maxRadius = std::max(0.0f, 0.5f - length * 0.55f);
        const f32 radius = std::sqrt(rng.next()) * maxRadius;
        const f32 angle = rng.next() * 6.2831853f;
        const Vec2 center { 0.5f + std::cos(angle) * radius,
                            0.5f + std::sin(angle) * radius };
        // Leaves lean outward from the cluster center, with jitter — reads
        // as a clump rather than confetti.
        const f32 lean = angle + rng.spread() * 0.9f;
        const f32 c = std::cos(lean);
        const f32 s = std::sin(lean);
        const u8 shade = static_cast<u8>(rng.next() * 255.0f);

        const f32 reach = length * 0.5f + 1.5f / fsize; // + AA margin
        const i32 x0 = std::max(0, static_cast<i32>((center.x - reach) * fsize));
        const i32 x1 = std::min(static_cast<i32>(size) - 1,
                                static_cast<i32>((center.x + reach) * fsize) + 1);
        const i32 y0 = std::max(0, static_cast<i32>((center.y - reach) * fsize));
        const i32 y1 = std::min(static_cast<i32>(size) - 1,
                                static_cast<i32>((center.y + reach) * fsize) + 1);
        for (i32 py = y0; py <= y1; ++py) {
            for (i32 px = x0; px <= x1; ++px) {
                const Vec2 p { (static_cast<f32>(px) + 0.5f) / fsize - center.x,
                               (static_cast<f32>(py) + 0.5f) / fsize -
                                   center.y };
                // Leaf frame: x along the axis in [0, length].
                const f32 lx = p.x * c + p.y * s + length * 0.5f;
                const f32 ly = -p.x * s + p.y * c;
                if (lx < 0.0f || lx > length) {
                    continue;
                }
                // Outline profile along the axis, per shape family.
                const f32 t = lx / length;
                const f32 arc = std::sin(t * 3.1415927f);
                f32 profile = std::pow(arc, 0.75f); // pointed ellipse
                if (shape == 2) {
                    profile = std::pow(arc, 0.35f); // rounded, blunt
                } else if (shape == 3) {
                    // Lobed (maple-ish): three bulges along the axis.
                    profile = std::pow(arc, 0.6f) *
                              (0.62f + 0.38f * std::abs(std::sin(
                                                    t * 9.42478f)));
                } else if (shape == 4) {
                    // Serrated: fine teeth riding the pointed outline.
                    profile = std::pow(arc, 0.75f) *
                              (0.82f + 0.18f * std::abs(std::sin(
                                                    t * 31.4159f)));
                }
                const f32 halfWidth = width * 0.5f * profile;
                // AA over one texel around the edge.
                const f32 coverage = glm::clamp(
                    (halfWidth - std::abs(ly)) * fsize + 0.5f, 0.0f, 1.0f);
                if (coverage <= 0.0f) {
                    continue;
                }
                u8* texel =
                    &pixels[(static_cast<size_t>(py) * size + px) * 4];
                const u8 alpha = static_cast<u8>(coverage * 255.0f);
                if (alpha >= texel[3]) {
                    texel[0] = shade;
                }
                texel[3] = std::max(texel[3], alpha);
            }
        }
    }
    return pixels;
}

} // namespace render
