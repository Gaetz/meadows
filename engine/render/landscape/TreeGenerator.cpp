#include "engine/render/landscape/TreeGenerator.hpp"

#include <cmath>

#include "engine/core/Hash.hpp"
#include "engine/render/MeshBuilder.hpp"

namespace render {

namespace {

// hashU32 / HashRng now live in engine/core/Hash.hpp (shared scatter hash family).
using core::hashU32;
using core::HashRng;

} // namespace

MeshData generateTree(u32 seed, u32 lobeSubdivisions,
                      const LobeTreeParams& params) {
    HashRng rng { hashU32(seed ^ 0x7ea3c1b9u) };
    MeshData mesh;

    // Tall, elegant silhouette: one proper trunk
    // column, short upward branches near the top, one full foliage lobe
    // per branch tip plus a crown (solid canopy, no cards).
    // Every artistic constant rides LobeTreeParams —
    // defaults reproduce the shipped look.
    const f32 trunkHeight =
        params.trunkHeightMin +
        rng.next() * (params.trunkHeightMax - params.trunkHeightMin);
    const f32 trunkRadius =
        params.trunkRadiusMin +
        rng.next() * (params.trunkRadiusMax - params.trunkRadiusMin);
    const Vec3 lean { (rng.next() - 0.5f) * params.lean, 1.0f,
                      (rng.next() - 0.5f) * params.lean };
    const Vec3 up = glm::normalize(lean);
    const Vec3 trunkTop = up * trunkHeight;
    const Vec3 trunkColor { 0.085f, 0.048f, 0.026f }; // linear bark brown
    appendTaperedTube(mesh, Vec3 { 0.0f }, trunkTop, trunkRadius,
                      trunkRadius * params.trunkTaper, 6, trunkColor);
    const u32 trunkVertexCount = static_cast<u32>(mesh.vertices.size());

    // Foliage lobes: one crown on the trunk top, one at each branch tip.
    struct Lobe {
        Vec3 center;
        f32 radius;
        u32 firstVertex;
    };
    Lobe lobes[7];
    u32 lobeCount = 0;
    lobes[lobeCount++] = { trunkTop + up * 0.45f,
                           params.crownLobeRadiusMin +
                               rng.next() * (params.crownLobeRadiusMax -
                                             params.crownLobeRadiusMin),
                           0 };

    // Short branches spread around the upper trunk, tilted well upward,
    // staggered in height so the canopy layers instead of forming a ball.
    const i32 branchSpan =
        glm::max(params.branchCountMax - params.branchCountMin + 1, 1);
    const u32 branchCount = static_cast<u32>(glm::clamp(
        params.branchCountMin +
            static_cast<i32>(rng.next() * static_cast<f32>(branchSpan)),
        1, 6)); // the lobes array holds crown + 6
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
        const f32 length =
            params.branchLengthMin +
            rng.next() * (params.branchLengthMax - params.branchLengthMin);
        const f32 branchRadius =
            trunkRadius * (0.40f + rng.next() * 0.15f);
        const Vec3 branchTip = branchBase + dir * length;
        appendTaperedTube(mesh, branchBase, branchTip, branchRadius,
                          branchRadius * 0.4f, 5, trunkColor);
        // The lobe sits mostly beyond the tip, hiding the joint.
        const f32 radius =
            params.branchLobeRadiusMin +
            rng.next() * (params.branchLobeRadiusMax -
                          params.branchLobeRadiusMin);
        lobes[lobeCount++] = { branchTip + dir * (radius * 0.35f), radius,
                               0 };
    }

    // The lobes ARE the canopy now: rounder spheres (subdiv 2 = 320 faces)
    // with soft jitter, full greens (no more x0.55 interior darkening).
    Vec3 canopyCenter { 0.0f };
    for (u32 i = 0; i < lobeCount; ++i) {
        canopyCenter += lobes[i].center;
    }
    canopyCenter /= static_cast<f32>(lobeCount);
    for (u32 i = 0; i < lobeCount; ++i) {
        const f32 hue = rng.next();
        // Linear greens centered on the meadow color (#6FA160 —
        // grassAlbedo), one hue roll per lobe so the canopy isn't flat.
        const Vec3 leafColor = glm::mix(Vec3 { 0.135f, 0.340f, 0.115f },
                                        Vec3 { 0.180f, 0.375f, 0.117f }, hue);
        const f32 jitter = 0.10f + rng.next() * 0.04f; // soft, not craggy
        lobes[i].firstVertex = static_cast<u32>(mesh.vertices.size());
        appendBlob(mesh, hashU32(seed ^ (0x51bd1e95u + i)), lobes[i].center,
                   lobes[i].radius, jitter, leafColor, lobeSubdivisions);
        // Flatten the lobe a touch (spread crowns, not balls), then
        // SPHERIZE the normals on the mesh: blend the lobe-center
        // direction with the whole-canopy direction — light pools per
        // lobe, coherent across the tree (halisavakis/BotW, on geometry).
        for (u32 v = lobes[i].firstVertex;
             v < static_cast<u32>(mesh.vertices.size()); ++v) {
            MeshVertex& vertex = mesh.vertices[v];
            vertex.position.y =
                lobes[i].center.y +
                (vertex.position.y - lobes[i].center.y) *
                    params.lobeFlatten;
            const Vec3 fromLobe =
                glm::normalize(vertex.position - lobes[i].center);
            const Vec3 fromCanopy =
                glm::normalize(vertex.position - canopyCenter);
            vertex.normal = glm::normalize(
                glm::mix(fromLobe, fromCanopy, params.normalSpherize));
        }
    }

    // Wind weights via uv (trunk rooted, canopy sways) + the vertical
    // color gradient: +25% toward the treetop, sunlit crowns for free.
    const f32 totalHeight = trunkHeight + 2.0f;
    for (u32 i = 0; i < mesh.vertices.size(); ++i) {
        MeshVertex& vertex = mesh.vertices[i];
        const f32 height01 =
            glm::clamp(vertex.position.y / totalHeight, 0.0f, 1.0f);
        const bool isTrunk = i < trunkVertexCount;
        const f32 sway = isTrunk ? height01 * 0.25f
                                 : 0.5f + height01 * 0.5f;
        // Trunk uv.y < -0.5 = bark flag (triplanar in tree.frag).
        vertex.uv = { sway, isTrunk ? -1.0f : height01 };
        if (!isTrunk) {
            vertex.color *= 1.0f + 0.25f * height01;
        }
    }
    return mesh;
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
