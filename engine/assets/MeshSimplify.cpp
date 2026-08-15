#include "engine/assets/MeshSimplify.hpp"

#include <meshoptimizer.h>

#include "engine/core/Log.hpp"

namespace assets {

void simplifyMesh(render::MeshData& mesh, u32 targetTriangles) {
    const size_t indexCount = mesh.indices.size();
    if (indexCount / 3 <= targetTriangles || mesh.vertices.empty()) {
        return;
    }
    const size_t targetIndices = static_cast<size_t>(targetTriangles) * 3;
    const f32* positions = &mesh.vertices[0].position.x;
    const size_t stride = sizeof(render::MeshVertex);

    vector<u32> simplified(indexCount);
    f32 error = 0.0f;
    // UV-aware collapse: the position-only path merged vertices across
    // the scans' UV-atlas seams — one side of a boulder ended with
    // stretched, striped texels ("la texture n'en fait pas le tour").
    // The uv attribute weight keeps island boundaries intact.
    const f32* uvs = &mesh.vertices[0].uv.x;
    const f32 kUvWeight[2] = { 1.0f, 1.0f };
    size_t count = meshopt_simplifyWithAttributes(
        simplified.data(), mesh.indices.data(), indexCount, positions,
        mesh.vertices.size(), stride, uvs, stride, kUvWeight, 2, nullptr,
        targetIndices, 0.05f, 0, &error);
    if (count > targetIndices * 3 / 2) {
        // Give the error budget room before surrendering topology.
        count = meshopt_simplifyWithAttributes(
            simplified.data(), mesh.indices.data(), indexCount,
            positions, mesh.vertices.size(), stride, uvs, stride,
            kUvWeight, 2, nullptr, targetIndices, 0.3f, 0, &error);
    }
    if (count > targetIndices * 3 / 2) {
        // Topology resisted (scans are full of tiny disconnected shells):
        // the sloppy simplifier ignores topology and always converges —
        // last resort only, it is what caused the seam smearing.
        count = meshopt_simplifySloppy(simplified.data(),
                                       mesh.indices.data(), indexCount,
                                       positions, mesh.vertices.size(),
                                       stride, targetIndices, 0.1f, &error);
    }
    simplified.resize(count);

    // Compact the vertex buffer to the surviving set.
    vector<render::MeshVertex> packed(mesh.vertices.size());
    const size_t unique = meshopt_optimizeVertexFetch(
        packed.data(), simplified.data(), count, mesh.vertices.data(),
        mesh.vertices.size(), stride);
    packed.resize(unique);
    mesh.vertices = std::move(packed);
    mesh.indices = std::move(simplified);
    LOG_INFO("simplifyMesh: {} tris -> {} ({} verts)", indexCount / 3,
             count / 3, unique);
}

} // namespace assets
