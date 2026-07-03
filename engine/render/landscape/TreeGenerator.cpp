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

} // namespace

MeshData generateTree(u32 seed) {
    HashRng rng { hashU32(seed ^ 0x7ea3c1b9u) };
    MeshData mesh;

    // Trunk: tapered, slightly leaning hexagonal tube.
    const f32 trunkHeight = 2.1f + rng.next() * 1.3f;
    const f32 trunkRadius = 0.13f + rng.next() * 0.07f;
    const Vec3 lean { (rng.next() - 0.5f) * 0.45f, 1.0f,
                      (rng.next() - 0.5f) * 0.45f };
    const Vec3 trunkTop = glm::normalize(lean) * trunkHeight;
    const Vec3 trunkColor { 0.085f, 0.048f, 0.026f }; // linear bark brown
    appendTaperedTube(mesh, Vec3 { 0.0f }, trunkTop, trunkRadius,
                      trunkRadius * 0.55f, 6, trunkColor);
    const u32 trunkVertexCount = static_cast<u32>(mesh.vertices.size());

    // Canopy: 3-4 faceted blobs clustered above the trunk, shrinking upward.
    const u32 blobCount = 3 + (rng.next() > 0.5f ? 1u : 0u);
    const Vec3 canopyCenter = trunkTop + Vec3 { 0.0f, 0.55f, 0.0f };
    for (u32 i = 0; i < blobCount; ++i) {
        const f32 lift = static_cast<f32>(i) / static_cast<f32>(blobCount);
        const Vec3 offset { (rng.next() - 0.5f) * 1.5f * (1.0f - lift),
                            lift * 1.1f,
                            (rng.next() - 0.5f) * 1.5f * (1.0f - lift) };
        const f32 radius = (1.15f + rng.next() * 0.45f) * (1.0f - lift * 0.4f);
        // Linear greens, one hue roll per blob so the canopy isn't flat.
        const f32 hue = rng.next();
        const Vec3 leafColor = glm::mix(Vec3 { 0.030f, 0.095f, 0.018f },
                                        Vec3 { 0.075f, 0.130f, 0.020f }, hue);
        appendBlob(mesh, hashU32(seed ^ (0x51bd1e95u + i)),
                   canopyCenter + offset, radius, 0.22f, leafColor);
    }

    // Wind weights via uv: the trunk stays rooted, foliage sways — weight
    // and height ramp with altitude along the tree.
    const f32 totalHeight = canopyCenter.y + 2.2f;
    for (u32 i = 0; i < mesh.vertices.size(); ++i) {
        MeshVertex& vertex = mesh.vertices[i];
        const f32 height01 =
            glm::clamp(vertex.position.y / totalHeight, 0.0f, 1.0f);
        const f32 sway = i < trunkVertexCount ? height01 * 0.25f
                                              : 0.5f + height01 * 0.5f;
        vertex.uv = { sway, height01 };
    }
    return mesh;
}

} // namespace render
