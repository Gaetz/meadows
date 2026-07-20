#include "engine/render/landscape/TreeGenerator.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/Hash.hpp"
#include "engine/render/MeshBuilder.hpp"

// EXPERIMENT (feature/space-colonization-trees) — Runions et al. 2007,
// "Modeling Trees with a Space Colonization Algorithm" + the cross-plane
// foliage variant suggested 2026-07-19 (metaball SDF at branch tips,
// order-weighted radii, card normals from the SDF gradient).
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

constexpr f32 kSegment = 0.28f;        // D — growth step (m)
constexpr f32 kKillDistance = 0.70f;   // d_k = 2.5 D
constexpr u32 kAttractorCount = 350;   // N — enough for a clean mid tree
constexpr u32 kMaxIterations = 120;
constexpr f32 kPipeExponent = 2.6f;    // pipe model n (2..3, Mac83)
constexpr f32 kTipRadius = 0.020f;     // r0 at every branch tip (m)
constexpr f32 kSmoothK = 0.7f;         // metaball smooth-min width (m)

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

f32 canopySdf(const vector<Metaball>& balls, const Vec3& p) {
    f32 d = 1e9f;
    for (const Metaball& ball : balls) {
        d = smoothMin(d, glm::length(p - ball.center) - ball.radius,
                      kSmoothK);
    }
    return d;
}

Vec3 canopySdfGradient(const vector<Metaball>& balls, const Vec3& p) {
    constexpr f32 kEps = 0.15f; // wide taps: SMOOTH normals are the point
    const f32 dx = canopySdf(balls, p + Vec3 { kEps, 0.0f, 0.0f }) -
                   canopySdf(balls, p - Vec3 { kEps, 0.0f, 0.0f });
    const f32 dy = canopySdf(balls, p + Vec3 { 0.0f, kEps, 0.0f }) -
                   canopySdf(balls, p - Vec3 { 0.0f, kEps, 0.0f });
    const f32 dz = canopySdf(balls, p + Vec3 { 0.0f, 0.0f, kEps }) -
                   canopySdf(balls, p - Vec3 { 0.0f, 0.0f, kEps });
    const Vec3 g { dx, dy, dz };
    const f32 len = glm::length(g);
    return len > 1e-6f ? g / len : Vec3 { 0.0f, 1.0f, 0.0f };
}

// One BILLBOARD leaf card (dev feedback 2026-07-20: a single card facing
// the player beats the mechanical 60° cross). The mesh stores a
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

} // namespace

MeshData generateColonizedTree(u32 seed, u32 detail, f32 foliageDensity) {
    // Independent streams so the SKELETON never depends on `detail` —
    // the three LODs of one seed must share silhouette and colors.
    HashRng shapeRng { hashU32(seed ^ 0x5c01a11eu) };
    HashRng scatterRng { hashU32(seed ^ 0xf011a9e5u) };
    MeshData mesh;

    // --- Crown envelope + attraction points ------------------------------
    const f32 trunkBase = 1.6f + shapeRng.next() * 0.9f;  // bare trunk (m)
    const f32 crownHeight = 2.6f + shapeRng.next() * 1.2f;
    const f32 crownRadius = 1.9f + shapeRng.next() * 1.1f;
    const Vec3 crownCenter { (shapeRng.next() - 0.5f) * 0.5f,
                             trunkBase + crownHeight * 0.55f,
                             (shapeRng.next() - 0.5f) * 0.5f };
    const f32 totalHeight = trunkBase + crownHeight + 0.4f;

    vector<Vec3> attractors;
    attractors.reserve(kAttractorCount);
    while (attractors.size() < kAttractorCount) {
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
    const Vec3 tropism { 0.0f, 0.14f, 0.0f }; // slight upward bias (eq. 3)
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
                nodes[n].position + direction * kSegment;
            // Degenerate-growth guard: don't stack a node onto a sibling.
            bool duplicate = false;
            for (u32 m = nodeCountBefore; m < nodes.size(); ++m) {
                if (glm::dot(position - nodes[m].position,
                             position - nodes[m].position) <
                    (0.5f * kSegment) * (0.5f * kSegment)) {
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
                    kKillDistance * kKillDistance) {
                    return true;
                }
            }
            return false;
        });
        if (attractors.size() < kAttractorCount / 20) {
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
            std::pow(parent.radius, kPipeExponent) +
                std::pow(nodes[n].radius, kPipeExponent),
            1.0f / kPipeExponent);
    }
    for (Node& node : nodes) {
        node.radius = glm::min(node.radius, 0.30f);
    }

    // --- Wood: chain-decimated tapered tubes ------------------------------
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
            f32 runLength = kSegment;
            while (nodes[runTop].parent > 0) {
                const u32 next = static_cast<u32>(nodes[runTop].parent);
                if (nodes[next].childCount != 1 ||
                    glm::dot(nodes[runTop].direction,
                             nodes[next].direction) < 0.95f ||
                    runLength > 1.1f) {
                    break;
                }
                runTop = next;
                runLength += kSegment;
            }
            const u32 top =
                nodes[runTop].parent >= 0
                    ? static_cast<u32>(nodes[runTop].parent)
                    : 0u;
            // Twig culling by LOD: past the near level, sub-centimeter
            // wood reads as noise and costs like geometry.
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
    const u32 woodVertexCount = static_cast<u32>(mesh.vertices.size());

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
            0.95f * std::pow(0.78f, static_cast<f32>(node.order)), 0.30f,
            1.05f);
        balls.push_back({ node.position, radius });
    }
    if (balls.empty()) { // degenerate seed: keep the mesh valid
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

    // --- Foliage: billboard leaf cards in the SDF shell -------------------
    // Dev feedback 2026-07-20 (bis): ONE camera-facing card per clump
    // (the 60° crosses read mechanical), leaf-clump sized, shell
    // TIGHTENED against the surface — the silhouette is where cards pay,
    // and a sparse interior never shows.
    const u32 baseClusters = detail >= 2 ? 560u : detail == 1 ? 260u : 120u;
    const u32 clusterCount = glm::clamp(
        static_cast<u32>(static_cast<f32>(baseClusters) *
                         glm::clamp(foliageDensity, 0.1f, 8.0f)),
        1u, 4000u);
    u32 emitted = 0;
    for (u32 c = 0; c < clusterCount * 4u && emitted < clusterCount; ++c) {
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
            const f32 d = canopySdf(balls, candidate);
            if (d > -0.30f && d < -0.02f) {
                position = candidate;
                break;
            }
        }

        const Vec3 normal = canopySdfGradient(balls, position);
        // Light-seeking density gradient (dev 2026-07-20): the card
        // count is FIXED (the loop draws candidates until clusterCount
        // land — same cost), but acceptance follows the canopy's outward
        // direction: exp2(normal.y) = x2 where it faces up, x1 on the
        // sides, x0.5 underneath. Leaves seek light; undersides thin out.
        const f32 weight = std::exp2(normal.y); // 0.5 .. 2.0
        if (scatterRng.next() * 2.0f > weight) {
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
        const f32 halfSize = 0.042f + scatterRng.next() * 0.030f;
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

} // namespace render
