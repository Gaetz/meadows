#include "engine/assets/VertexAo.hpp"

#include <cmath>

#include <glm/glm.hpp>

namespace assets {

namespace {

// Möller–Trumbore, front+back faces (an occluder occludes either way).
bool rayTriangle(const Vec3& origin, const Vec3& direction, const Vec3& a,
                 const Vec3& b, const Vec3& c, f32& t) {
    const Vec3 e1 = b - a;
    const Vec3 e2 = c - a;
    const Vec3 p = glm::cross(direction, e2);
    const f32 det = glm::dot(e1, p);
    if (std::abs(det) < 1e-7f) {
        return false;
    }
    const f32 inv = 1.0f / det;
    const Vec3 s = origin - a;
    const f32 u = glm::dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const Vec3 q = glm::cross(s, e1);
    const f32 v = glm::dot(direction, q) * inv;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    t = glm::dot(e2, q) * inv;
    return t > 1e-4f;
}

} // namespace

void bakeVertexAo(render::MeshData& mesh, f32 strength, u32 rayCount,
                  f32 maxDistance) {
    if (strength <= 0.0f || rayCount == 0 || mesh.indices.size() < 3) {
        return;
    }
    const f32 goldenAngle = 2.3999632f;
    for (render::MeshVertex& vertex : mesh.vertices) {
        const Vec3 normal = glm::normalize(vertex.normal);
        const Vec3 helper = std::abs(normal.y) < 0.9f
                                ? Vec3 { 0.0f, 1.0f, 0.0f }
                                : Vec3 { 1.0f, 0.0f, 0.0f };
        const Vec3 tangent = glm::normalize(glm::cross(helper, normal));
        const Vec3 bitangent = glm::cross(normal, tangent);
        // Nudge off the surface so the vertex's own faces don't self-hit.
        const Vec3 origin = vertex.position + normal * 0.01f;

        f32 occlusion = 0.0f;
        for (u32 i = 0; i < rayCount; ++i) {
            // Golden-spiral hemisphere (deterministic — the bake must be
            // stable across runs and machines, §8 spirit).
            const f32 z = (static_cast<f32>(i) + 0.5f) /
                          static_cast<f32>(rayCount);
            const f32 r = std::sqrt(std::max(0.0f, 1.0f - z * z));
            const f32 angle = goldenAngle * static_cast<f32>(i);
            const Vec3 direction = tangent * (r * std::cos(angle)) +
                                   bitangent * (r * std::sin(angle)) +
                                   normal * z;
            // Nearest self-hit within reach.
            f32 nearest = maxDistance;
            bool hit = false;
            for (size_t tri = 0; tri + 2 < mesh.indices.size(); tri += 3) {
                const Vec3& a = mesh.vertices[mesh.indices[tri]].position;
                const Vec3& b =
                    mesh.vertices[mesh.indices[tri + 1]].position;
                const Vec3& c =
                    mesh.vertices[mesh.indices[tri + 2]].position;
                f32 t = 0.0f;
                if (rayTriangle(origin, direction, a, b, c, t) &&
                    t < nearest) {
                    nearest = t;
                    hit = true;
                }
            }
            if (hit) {
                occlusion += 1.0f - nearest / maxDistance; // close = dark
            }
        }
        const f32 ao = glm::clamp(
            1.0f - strength * (occlusion / static_cast<f32>(rayCount)),
            0.0f, 1.0f);
        vertex.color *= ao;
    }
}

} // namespace assets
