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

void appendBillboardCard(MeshData& mesh, const Vec3& center, f32 halfSize,
                         const Vec3& normal, const Vec3& color) {
    const u32 base = static_cast<u32>(mesh.vertices.size());
    for (const Vec2 corner : { Vec2 { -1.0f, -1.0f }, Vec2 { 1.0f, -1.0f },
                               Vec2 { 1.0f, 1.0f }, Vec2 { -1.0f, 1.0f } }) {
        mesh.vertices.push_back(
            { center, normal,
              { kCardFlagBias + corner.x * halfSize,
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
        // Rejection-sample the ellipsoid (deterministic sequence).
        const Vec3 unit { shapeRng.next() * 2.0f - 1.0f,
                          shapeRng.next() * 2.0f - 1.0f,
                          shapeRng.next() * 2.0f - 1.0f };
        if (glm::dot(unit, unit) > 1.0f) {
            continue;
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
            const Vec3 direction =
                glm::normalize(pullSum[n] / pullLen + tropism);
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
            0.30f, glm::max(params.tipBallRadius, 0.35f));
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

// Wood: chain-decimated tapered tubes. `detail` picks tube sides and the
// twig-culling radius floor (past the near level, sub-centimeter wood
// reads as noise and costs like geometry).
void appendWood(MeshData& mesh, const vector<Node>& nodes, u32 detail,
                f32 segment) {
    const Vec3 barkColor { 0.085f, 0.048f, 0.026f }; // generateTree's bark
    const u32 tubeSides = detail >= 2 ? 5u : detail == 1 ? 4u : 3u;
    // Walk each chain from a branching point (or tip) down to the previous
    // branching point, emitting a tube per decimated run: consecutive
    // near-collinear single-child nodes collapse into one segment.
    for (u32 n = 1; n < nodes.size(); ++n) {
        const bool chainEnd =
            nodes[n].childCount != 1; // tip or branching point
        if (!chainEnd) {
            continue;
        }
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
                             nodes[next].direction) < 0.95f ||
                    runLength > 1.1f) {
                    break;
                }
                runTop = next;
                runLength += segment;
            }
            const u32 top =
                nodes[runTop].parent >= 0
                    ? static_cast<u32>(nodes[runTop].parent)
                    : 0u;
            const f32 minRadius =
                detail >= 2 ? 0.012f : detail == 1 ? 0.025f : 0.045f;
            if (nodes[cursor].radius >= minRadius) {
                appendTaperedTube(mesh, nodes[top].position,
                                  nodes[cursor].position,
                                  nodes[top].radius, nodes[cursor].radius,
                                  tubeSides, barkColor);
            }
            cursor = top;
            if (nodes[cursor].childCount > 1 || cursor == 0) {
                break; // the parent chain is someone else's walk
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
    appendWood(mesh, tree.nodes, detail, params.segment);
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
    u32 emitted = 0;
    for (u32 c = 0; c < clusterCount * 6u && emitted < clusterCount; ++c) {
        // The scatter stream runs the SAME sequence at every detail level
        // (clusterCount only truncates it): LODs agree on where the
        // canopy mass sits.
        const Metaball& ball =
            balls[static_cast<u32>(scatterRng.next() *
                                   static_cast<f32>(balls.size())) %
                  balls.size()];
        Vec3 position = ball.center;
        for (u32 attempt = 0; attempt < 24; ++attempt) {
            const Vec3 candidate =
                ball.center + Vec3 { (scatterRng.next() * 2.0f - 1.0f),
                                     (scatterRng.next() * 2.0f - 1.0f),
                                     (scatterRng.next() * 2.0f - 1.0f) } *
                                  (ball.radius * 1.10f);
            const f32 d = canopySdf(balls, candidate, params.smoothK);
            if (d > -0.30f && d < -0.02f) {
                position = candidate;
                break;
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
        Vec3 leafColor =
            glm::mix(Vec3 { 0.030f, 0.095f, 0.018f },
                     Vec3 { 0.075f, 0.130f, 0.020f }, hue); // tree palette
        // Sunlit-crown gradient, baked here (the uv loop below is
        // wood-only now — card uv carries the billboard corner).
        leafColor *= 1.0f + 0.25f * glm::clamp(position.y / totalHeight,
                                               0.0f, 1.0f);
        const f32 halfSize =
            params.cardHalfSizeMin +
            scatterRng.next() *
                (params.cardHalfSizeMax - params.cardHalfSizeMin);
        appendBillboardCard(mesh, position, halfSize, normal, leafColor);
        ++emitted;
    }

    // --- Wind weights + vertical gradient — WOOD ONLY (card uv is the
    // billboard encoding; tree.vert gives cards a fixed sway weight). ----
    for (u32 i = 0; i < woodVertexCount; ++i) {
        MeshVertex& vertex = mesh.vertices[i];
        const f32 height01 =
            glm::clamp(vertex.position.y / totalHeight, 0.0f, 1.0f);
        vertex.uv = { height01 * 0.30f, height01 };
    }
    return mesh;
}

MeshData generateColonizedTreeShadowProxy(u32 seed,
                                          const ColonizedTreeParams& params) {
    const GrownTree tree = growColonizedTree(seed, params);
    MeshData mesh;
    appendWood(mesh, tree.nodes, 0, params.segment);
    // The canopy metaballs ARE the shadow volume: one 20-face icosahedron
    // per ball (largest first — they define the mass) instead of the card
    // cloud. Opaque, so shadow_prop skips the leaf-mask cutout entirely.
    const Vec3 leafColor { 0.045f, 0.105f, 0.019f };
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
                                  const ColonizedTreeParams& params) {
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
        const f32 width = length * (0.34f + rng.next() * 0.14f);
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
                // Pointed at both ends: half-width follows sin^0.75.
                const f32 t = lx / length;
                const f32 halfWidth =
                    width * 0.5f *
                    std::pow(std::sin(t * 3.1415927f), 0.75f);
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
