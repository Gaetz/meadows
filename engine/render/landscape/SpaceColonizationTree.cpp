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

// One double-sided quad (two windings, SAME normal — the SDF shading must
// hold from every side). `right`/`up` are half-extent edge vectors.
void appendCard(MeshData& mesh, const Vec3& center, const Vec3& right,
                const Vec3& up, const Vec3& normal, const Vec3& color) {
    const Vec3 p0 = center - right - up;
    const Vec3 p1 = center + right - up;
    const Vec3 p2 = center + right + up;
    const Vec3 p3 = center - right + up;
    const auto vertex = [&](const Vec3& p) {
        return MeshVertex { p, normal, { 0.0f, 0.0f }, color };
    };
    const u32 base = static_cast<u32>(mesh.vertices.size());
    mesh.vertices.push_back(vertex(p0));
    mesh.vertices.push_back(vertex(p1));
    mesh.vertices.push_back(vertex(p2));
    mesh.vertices.push_back(vertex(p3));
    // Front winding then back winding — pipeline culls back faces.
    const u32 quads[12] = { 0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2 };
    for (u32 i : quads) {
        mesh.indices.push_back(base + i);
    }
}

} // namespace

MeshData generateColonizedTree(u32 seed, u32 detail) {
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
            const u32 parent = static_cast<u32>(nodes[cursor].parent);
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

    // --- Foliage: cross-plane clusters in the SDF shell -------------------
    // Dev feedback 2026-07-20: leaf-CLUMP sized cards, lots of them —
    // 1/10 the size, 10x the count of the first cut (huge planes read as
    // paper; ~1 m clumps on a x8 tree read as foliage).
    const u32 clusterCount = detail >= 2 ? 560u : detail == 1 ? 260u : 120u;
    constexpr f32 kTau = 6.2831853f;
    for (u32 c = 0; c < clusterCount; ++c) {
        // The scatter stream runs the SAME sequence at every detail level
        // (clusterCount only truncates it): LODs agree on where the
        // canopy mass sits.
        const Metaball& ball =
            balls[static_cast<u32>(scatterRng.next() *
                                   static_cast<f32>(balls.size())) %
                  balls.size()];
        Vec3 position = ball.center;
        for (u32 attempt = 0; attempt < 16; ++attempt) {
            const Vec3 candidate =
                ball.center + Vec3 { (scatterRng.next() * 2.0f - 1.0f),
                                     (scatterRng.next() * 2.0f - 1.0f),
                                     (scatterRng.next() * 2.0f - 1.0f) } *
                                  (ball.radius * 1.15f);
            const f32 d = canopySdf(balls, candidate);
            // Shell bias: clusters live near the surface — the silhouette
            // is where cards pay, the deep interior is never seen.
            if (d > -0.60f && d < -0.06f) {
                position = candidate;
                break;
            }
        }

        const f32 hue = scatterRng.next();
        const Vec3 leafColor =
            glm::mix(Vec3 { 0.030f, 0.095f, 0.018f },
                     Vec3 { 0.075f, 0.130f, 0.020f }, hue); // tree palette
        const Vec3 normal = canopySdfGradient(balls, position);
        const f32 size = 0.042f + scatterRng.next() * 0.030f;
        const f32 baseYaw = scatterRng.next() * kTau;
        const f32 tilt = (scatterRng.next() - 0.5f) * 0.7f;
        // Three planes crossed at 60°, all shading with the SDF normal.
        for (u32 plane = 0; plane < 3; ++plane) {
            const f32 yaw =
                baseYaw + static_cast<f32>(plane) * (kTau / 6.0f);
            const Vec3 right { std::cos(yaw) * size, std::sin(tilt) * 0.2f,
                               std::sin(yaw) * size };
            const Vec3 planeUp { -std::sin(yaw) * std::sin(tilt) * size *
                                     0.35f,
                                 size * 0.85f,
                                 std::cos(yaw) * std::sin(tilt) * size *
                                     0.35f };
            appendCard(mesh, position, right, planeUp, normal, leafColor);
        }
    }

    // --- Wind weights + vertical gradient (the generateTree contract) ----
    for (u32 i = 0; i < mesh.vertices.size(); ++i) {
        MeshVertex& vertex = mesh.vertices[i];
        const f32 height01 =
            glm::clamp(vertex.position.y / totalHeight, 0.0f, 1.0f);
        const bool isWood = i < woodVertexCount;
        const f32 sway = isWood ? height01 * 0.30f : 0.5f + height01 * 0.5f;
        vertex.uv = { sway, height01 };
        if (!isWood) {
            vertex.color *= 1.0f + 0.25f * height01; // sunlit crown
        }
    }
    return mesh;
}

} // namespace render
