#include "engine/render/landscape/TreeGenerator.hpp"

#include <cmath>

#include "engine/render/MeshBuilder.hpp"

namespace render {

namespace {

u32 hashU32(u32 v) {
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}

struct HashRng {
    u32 state;
    f32 next() { // [0, 1)
        state = hashU32(state);
        return static_cast<f32>(state) * (1.0f / 4294967296.0f);
    }
};

// One leaf card: a STATIC quad (the article's construction — Habrador /
// MTree-style placement), roughly tangent to the blob surface at `dir`,
// random roll and tilt. Static beats billboards on fill-rate: a fixed quad
// presents ~half its area on average, a billboard always presents all of
// it. Every corner carries the SPHERICAL normal (blob-center direction) —
// the trick that lights the canopy as one soft volume.
void appendLeafCard(MeshData& mesh, HashRng& rng, const Vec3& blobCenter,
                    f32 blobRadius, const Vec3& dir, const Vec3& color) {
    // Tangent basis around the surface direction, random roll.
    const Vec3 helper = std::abs(dir.y) < 0.85f ? Vec3 { 0.0f, 1.0f, 0.0f }
                                                : Vec3 { 1.0f, 0.0f, 0.0f };
    const Vec3 tangentA = glm::normalize(glm::cross(dir, helper));
    const Vec3 tangentB = glm::cross(dir, tangentA);
    const f32 roll = rng.next() * 6.2831853f;
    const Vec3 right = tangentA * std::cos(roll) + tangentB * std::sin(roll);
    Vec3 up = glm::cross(dir, right);
    // Tilt off the tangent plane so silhouettes stay fluffy edge-on.
    up = glm::normalize(up + dir * ((rng.next() - 0.5f) * 0.9f));

    // Ride ON the surface, not in the mass: the blob's radial jitter bulges
    // up to ~1.2 × radius, so cards sit at 1.05-1.30 ×.
    const Vec3 center =
        blobCenter + dir * (blobRadius * (1.05f + rng.next() * 0.25f));
    // Mid-size cards (~0.35-0.6 m): big enough that each quad reads as a
    // leaf cluster, small enough not to plate the canopy in giant panels.
    const f32 halfSize = blobRadius * (0.22f + rng.next() * 0.12f);

    // One of the four atlas cells.
    const f32 cellU = rng.next() < 0.5f ? 0.0f : 0.5f;
    const f32 cellV = rng.next() < 0.5f ? 0.0f : 0.5f;

    const u32 base = static_cast<u32>(mesh.vertices.size());
    const Vec3 corners[4] = {
        center - right * halfSize - up * halfSize,
        center + right * halfSize - up * halfSize,
        center + right * halfSize + up * halfSize,
        center - right * halfSize + up * halfSize,
    };
    const Vec2 uvs[4] = { { cellU, cellV },
                          { cellU + 0.5f, cellV },
                          { cellU + 0.5f, cellV + 0.5f },
                          { cellU, cellV + 0.5f } };
    for (u32 i = 0; i < 4; ++i) {
        mesh.vertices.push_back({ .position = corners[i],
                                  .normal = dir,
                                  .uv = uvs[i],
                                  .color = color });
    }
    mesh.indices.insert(mesh.indices.end(),
                        { base, base + 1, base + 2, base, base + 2,
                          base + 3 });
}

} // namespace

TreeMeshes generateTree(u32 seed) {
    HashRng rng { hashU32(seed ^ 0x7ea3c1b9u) };
    TreeMeshes tree;
    MeshData& mesh = tree.body;

    // Tall, elegant silhouette (blog-article composition): one proper
    // trunk column, short upward branches near the top, and a foliage
    // blob at each branch tip carrying the leaf cards.
    const f32 trunkHeight = 4.2f + rng.next() * 1.9f; // 4.2–6.1 m
    const f32 trunkRadius = 0.17f + rng.next() * 0.07f;
    const Vec3 lean { (rng.next() - 0.5f) * 0.16f, 1.0f,
                      (rng.next() - 0.5f) * 0.16f };
    const Vec3 up = glm::normalize(lean);
    const Vec3 trunkTop = up * trunkHeight;
    const Vec3 trunkColor { 0.085f, 0.048f, 0.026f }; // linear bark brown
    appendTaperedTube(mesh, Vec3 { 0.0f }, trunkTop, trunkRadius,
                      trunkRadius * 0.42f, 6, trunkColor);
    const u32 trunkVertexCount = static_cast<u32>(mesh.vertices.size());

    // Foliage clumps: one crown on the trunk top, one at each branch tip.
    struct Clump {
        Vec3 center;
        f32 radius;
    };
    Clump clumps[7];
    u32 clumpCount = 0;
    clumps[clumpCount++] = { trunkTop + up * 0.45f,
                             0.85f + rng.next() * 0.40f };

    // Short branches spread around the upper trunk, tilted well upward,
    // staggered in height so the canopy layers instead of forming a ball.
    const u32 branchCount = 3 + static_cast<u32>(rng.next() * 3.0f); // 3-5
    for (u32 i = 0; i < branchCount; ++i) {
        const f32 along = 0.58f + 0.34f * (static_cast<f32>(i) + rng.next()) /
                                      static_cast<f32>(branchCount);
        const Vec3 branchBase = up * (trunkHeight * along);
        const f32 azimuth = (static_cast<f32>(i) + rng.next() * 0.6f) /
                                static_cast<f32>(branchCount) * 6.2831853f;
        const f32 tilt = 0.55f + rng.next() * 0.55f; // rad above horizontal
        const Vec3 dir = glm::normalize(
            Vec3 { std::cos(azimuth) * std::cos(tilt), std::sin(tilt),
                   std::sin(azimuth) * std::cos(tilt) });
        const f32 length = 0.9f + rng.next() * 0.8f;
        const f32 branchRadius =
            trunkRadius * (0.40f + rng.next() * 0.15f);
        const Vec3 branchTip = branchBase + dir * length;
        appendTaperedTube(mesh, branchBase, branchTip, branchRadius,
                          branchRadius * 0.4f, 5, trunkColor);
        // The clump sits mostly beyond the tip, hiding the joint.
        const f32 radius = 0.60f + rng.next() * 0.38f;
        clumps[clumpCount++] = { branchTip + dir * (radius * 0.35f), radius };
    }

    // Blobs (darkened: the leaf mass's shaded interior under the cards,
    // and the shadow casters) + leaf cards on each clump's surface.
    for (u32 i = 0; i < clumpCount; ++i) {
        const f32 hue = rng.next();
        // Linear greens, one hue roll per clump so the canopy isn't flat.
        const Vec3 leafColor = glm::mix(Vec3 { 0.030f, 0.095f, 0.018f },
                                        Vec3 { 0.075f, 0.130f, 0.020f }, hue);
        // 80 faces are enough: the card shell hides the blob, which only
        // fills gaps darkly (and casts the shadows).
        appendBlob(mesh, hashU32(seed ^ (0x51bd1e95u + i)), clumps[i].center,
                   clumps[i].radius, 0.20f, leafColor * 0.55f);

        HashRng cardRng { hashU32(seed ^ (0x9e3779b9u + i * 747796405u)) };
        const u32 cardCount = static_cast<u32>(
            12.566f * clumps[i].radius * clumps[i].radius *
            5.5f); // 4πr² × cards per m² — sized against the 0.22-0.34×r
                   // cards for ~full shell coverage
        const Vec3 cardBright = glm::mix(leafColor,
                                         Vec3 { 0.110f, 0.175f, 0.045f },
                                         0.5f);
        for (u32 c = 0; c < cardCount; ++c) {
            // Uniform sphere direction, then push toward +Y a touch.
            const f32 z = cardRng.next() * 2.0f - 1.0f;
            const f32 phi = cardRng.next() * 6.2831853f;
            const f32 s = std::sqrt(glm::max(1.0f - z * z, 0.0f));
            Vec3 dir { s * std::cos(phi), z, s * std::sin(phi) };
            dir = glm::normalize(dir + Vec3 { 0.0f, 0.35f, 0.0f });
            const Vec3 cardColor =
                glm::mix(leafColor, cardBright, cardRng.next());
            appendLeafCard(tree.leaves, cardRng, clumps[i].center,
                           clumps[i].radius, dir, cardColor);
        }
    }

    // Wind weights via uv: the trunk stays rooted, foliage sways — weight
    // and height ramp with altitude along the tree. (Leaf-card uvs address
    // the bouquet atlas instead; leaf.vert derives sway from height alone.)
    const f32 totalHeight = trunkHeight + 2.0f;
    for (u32 i = 0; i < mesh.vertices.size(); ++i) {
        MeshVertex& vertex = mesh.vertices[i];
        const f32 height01 =
            glm::clamp(vertex.position.y / totalHeight, 0.0f, 1.0f);
        const f32 sway = i < trunkVertexCount ? height01 * 0.25f
                                              : 0.5f + height01 * 0.5f;
        vertex.uv = { sway, height01 };
    }
    return tree;
}

MeshData generateRock(u32 seed) {
    HashRng rng { hashU32(seed ^ 0x94d049bbu) };
    MeshData mesh;

    const f32 radius = 0.55f + rng.next() * 0.5f;
    const f32 grayRoll = rng.next();
    const Vec3 rockColor = glm::mix(Vec3 { 0.105f, 0.100f, 0.095f },
                                    Vec3 { 0.150f, 0.145f, 0.140f }, grayRoll);
    appendBlob(mesh, hashU32(seed ^ 0xd3a2646cu),
               Vec3 { 0.0f, radius * 0.45f, 0.0f }, radius, 0.38f, rockColor);

    // Squash vertically for that sat-in-the-ground boulder silhouette, then
    // fix the face normals the squash bent.
    const f32 squash = 0.55f + rng.next() * 0.25f;
    for (MeshVertex& vertex : mesh.vertices) {
        vertex.position.y *= squash;
        vertex.uv = { 0.0f, 0.0f }; // rigid: no wind
    }
    recomputeFlatNormals(mesh);
    return mesh;
}

vector<u8> buildLeafTexturePixels() {
    constexpr u32 kSize = kLeafTextureSize;
    constexpr u32 kCell = kSize / 2;
    vector<u8> pixels(static_cast<size_t>(kSize) * kSize * 4, 0);

    // Luminance buffer per pixel; alpha composited painter-style so outer
    // (brighter) leaves overlap inner (darker) ones.
    const auto blendLeaf = [&](f32 px, f32 py, f32 angle, f32 halfLen,
                               f32 halfWidth, f32 lum) {
        const f32 c = std::cos(angle);
        const f32 s = std::sin(angle);
        const i32 reach = static_cast<i32>(halfLen + 2.0f);
        const i32 cx = static_cast<i32>(px);
        const i32 cy = static_cast<i32>(py);
        for (i32 y = cy - reach; y <= cy + reach; ++y) {
            for (i32 x = cx - reach; x <= cx + reach; ++x) {
                if (x < 0 || y < 0 || x >= static_cast<i32>(kSize) ||
                    y >= static_cast<i32>(kSize)) {
                    continue;
                }
                const f32 dx = static_cast<f32>(x) + 0.5f - px;
                const f32 dy = static_cast<f32>(y) + 0.5f - py;
                // Into the leaf's frame; slight point at the tip.
                const f32 u = dx * c + dy * s;
                const f32 v = -dx * s + dy * c;
                const f32 taper =
                    1.0f - 0.45f * glm::max(u / halfLen, 0.0f);
                const f32 d = (u * u) / (halfLen * halfLen) +
                              (v * v) /
                                  (halfWidth * halfWidth * taper * taper);
                if (d > 1.0f) {
                    continue;
                }
                // Crisp edge (the article look: alpha draws the shape,
                // lighting does the values), tiny rib highlight only.
                const f32 edge = glm::clamp((1.0f - d) * 9.0f, 0.0f, 1.0f);
                const f32 rib = 1.0f + 0.06f * (1.0f - std::abs(v) /
                                                           halfWidth);
                const size_t o =
                    (static_cast<size_t>(y) * kSize + x) * 4;
                const f32 a = edge;
                const f32 prevA = pixels[o + 3] / 255.0f;
                const f32 outA = a + prevA * (1.0f - a);
                if (outA <= 0.0f) {
                    continue;
                }
                const f32 prevL = pixels[o] / 255.0f;
                const f32 l =
                    (lum * rib * a + prevL * prevA * (1.0f - a)) / outA;
                const u8 lb = static_cast<u8>(
                    glm::clamp(l, 0.0f, 1.0f) * 255.0f);
                pixels[o] = lb;
                pixels[o + 1] = lb;
                pixels[o + 2] = lb;
                pixels[o + 3] = static_cast<u8>(outA * 255.0f);
            }
        }
    };

    for (u32 cellY = 0; cellY < 2; ++cellY) {
        for (u32 cellX = 0; cellX < 2; ++cellX) {
            HashRng rng { hashU32(0x1eafu ^ (cellY * 2u + cellX + 1u)) };
            const f32 centerX = (static_cast<f32>(cellX) + 0.5f) * kCell;
            const f32 centerY = (static_cast<f32>(cellY) + 0.5f) * kCell;
            const f32 maxR = kCell * 0.30f;
            // A HANDFUL of big overlapping leaves (the article's bouquet):
            // the cluster core is solid, only the rim shows individual leaf
            // silhouettes. Near-flat luminance — the cel lighting owns the
            // values, not per-pixel texture noise.
            constexpr u32 kLeaves = 16;
            for (u32 i = 0; i < kLeaves; ++i) {
                const f32 r01 = static_cast<f32>(i) / kLeaves;
                const f32 r = maxR * std::sqrt(r01) *
                              (0.55f + rng.next() * 0.45f);
                const f32 a = rng.next() * 6.2831853f;
                const f32 px = centerX + std::cos(a) * r;
                const f32 py = centerY + std::sin(a) * r;
                // Point outward from the bouquet center, jittered.
                const f32 angle = a + (rng.next() - 0.5f) * 1.2f;
                const f32 halfLen = kCell * (0.21f + rng.next() * 0.09f);
                const f32 halfWidth = halfLen * (0.45f + rng.next() * 0.15f);
                const f32 lum = 0.88f + 0.12f * rng.next();
                blendLeaf(px, py, angle, halfLen, halfWidth, lum);
            }
        }
    }
    return pixels;
}

MeshData generateBush(u32 seed) {
    HashRng rng { hashU32(seed ^ 0xbf58476du) };
    MeshData mesh;

    const u32 blobCount = 1 + (rng.next() > 0.45f ? 1u : 0u);
    for (u32 i = 0; i < blobCount; ++i) {
        const f32 radius = 0.45f + rng.next() * 0.35f;
        const Vec3 center { (rng.next() - 0.5f) * 0.6f, radius * 0.55f,
                            (rng.next() - 0.5f) * 0.6f };
        const f32 hue = rng.next();
        // A touch darker and bluer than the tree canopy.
        const Vec3 bushColor = glm::mix(Vec3 { 0.022f, 0.075f, 0.020f },
                                        Vec3 { 0.055f, 0.105f, 0.024f }, hue);
        appendBlob(mesh, hashU32(seed ^ (0x2ab7e151u + i)), center, radius,
                   0.28f, bushColor);
    }
    for (MeshVertex& vertex : mesh.vertices) {
        const f32 height01 = glm::clamp(vertex.position.y / 1.2f, 0.0f, 1.0f);
        vertex.uv = { 0.35f * height01, height01 }; // gentle rustle
    }
    return mesh;
}

} // namespace render
