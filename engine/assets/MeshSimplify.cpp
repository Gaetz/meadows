#include "engine/assets/MeshSimplify.hpp"

#include <meshoptimizer.h>

#include <glm/glm.hpp>

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
    size_t count = meshopt_simplify(
        simplified.data(), mesh.indices.data(), indexCount, positions,
        mesh.vertices.size(), stride, targetIndices, 0.05f, 0, &error);
    if (count > targetIndices * 3 / 2) {
        // Topology resisted (scans are full of tiny disconnected shells):
        // the sloppy simplifier ignores topology and always converges.
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

void normalizeMeshFootprint(render::MeshData& mesh, f32 size) {
    if (mesh.vertices.empty()) {
        return;
    }
    Vec3 lo = mesh.vertices[0].position;
    Vec3 hi = lo;
    for (const render::MeshVertex& v : mesh.vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
    const Vec3 extent = hi - lo;
    const f32 largest =
        glm::max(extent.x, glm::max(extent.y, extent.z));
    const f32 scale = largest > 1e-4f ? size / largest : 1.0f;
    const Vec3 center { (lo.x + hi.x) * 0.5f, lo.y,
                        (lo.z + hi.z) * 0.5f };
    for (render::MeshVertex& v : mesh.vertices) {
        v.position = (v.position - center) * scale;
    }
}

} // namespace assets
