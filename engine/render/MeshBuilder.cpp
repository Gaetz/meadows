#include "engine/render/MeshBuilder.hpp"

#include <cmath>
#include <unordered_map>

#include "engine/core/Hash.hpp"

namespace render {

namespace {

// hashU32 lives in engine/core/Hash.hpp (shared scatter hash family).
using core::hashU32;

// Stable per-direction hash: quantize the unit vector so equal directions
// (however they were computed) jitter identically.
f32 directionHash01(u32 seed, const Vec3& dir) {
    const i32 qx = static_cast<i32>(std::lround(dir.x * 512.0f));
    const i32 qy = static_cast<i32>(std::lround(dir.y * 512.0f));
    const i32 qz = static_cast<i32>(std::lround(dir.z * 512.0f));
    u32 h = seed;
    h = hashU32(h ^ static_cast<u32>(qx));
    h = hashU32(h ^ static_cast<u32>(qy));
    h = hashU32(h ^ static_cast<u32>(qz));
    return static_cast<f32>(h) * (1.0f / 4294967295.0f);
}

void appendTriangle(MeshData& mesh, const Vec3& a, const Vec3& b,
                    const Vec3& c, const Vec3& color) {
    const Vec3 normal = glm::normalize(glm::cross(b - a, c - a));
    const u32 baseIndex = static_cast<u32>(mesh.vertices.size());
    for (const Vec3& p : { a, b, c }) {
        mesh.vertices.push_back({ .position = p,
                                  .normal = normal,
                                  .uv = { 0.0f, 0.0f },
                                  .color = color });
    }
    mesh.indices.insert(mesh.indices.end(),
                        { baseIndex, baseIndex + 1, baseIndex + 2 });
}

} // namespace

void appendTaperedTube(MeshData& mesh, const Vec3& base, const Vec3& top,
                       f32 radiusBase, f32 radiusTop, u32 sides,
                       const Vec3& color) {
    const Vec3 axis = glm::normalize(top - base);
    // Any vector not parallel to the axis seeds the ring basis.
    const Vec3 helper = std::abs(axis.y) < 0.95f ? Vec3 { 0.0f, 1.0f, 0.0f }
                                                 : Vec3 { 1.0f, 0.0f, 0.0f };
    const Vec3 u = glm::normalize(glm::cross(axis, helper));
    const Vec3 v = glm::cross(axis, u);

    constexpr f32 kTau = 6.2831853f;
    for (u32 i = 0; i < sides; ++i) {
        const f32 a0 = kTau * static_cast<f32>(i) / static_cast<f32>(sides);
        const f32 a1 =
            kTau * static_cast<f32>(i + 1) / static_cast<f32>(sides);
        const Vec3 dir0 = u * std::cos(a0) + v * std::sin(a0);
        const Vec3 dir1 = u * std::cos(a1) + v * std::sin(a1);
        const Vec3 b0 = base + dir0 * radiusBase;
        const Vec3 b1 = base + dir1 * radiusBase;
        const Vec3 t0 = top + dir0 * radiusTop;
        const Vec3 t1 = top + dir1 * radiusTop;
        // Outward winding (CCW seen from outside).
        appendTriangle(mesh, b0, b1, t1, color);
        appendTriangle(mesh, b0, t1, t0, color);
    }
}

void appendBlob(MeshData& mesh, u32 seed, const Vec3& center, f32 radius,
                f32 jitter, const Vec3& color, u32 subdivisions) {
    // Icosahedron.
    const f32 phi = 1.6180339f;
    const Vec3 base[12] = {
        { -1, phi, 0 }, { 1, phi, 0 }, { -1, -phi, 0 }, { 1, -phi, 0 },
        { 0, -1, phi }, { 0, 1, phi }, { 0, -1, -phi }, { 0, 1, -phi },
        { phi, 0, -1 }, { phi, 0, 1 }, { -phi, 0, -1 }, { -phi, 0, 1 },
    };
    constexpr u32 kFaces[20][3] = {
        { 0, 11, 5 }, { 0, 5, 1 },  { 0, 1, 7 },   { 0, 7, 10 },
        { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 },  { 11, 10, 2 },
        { 10, 7, 6 },  { 7, 1, 8 }, { 3, 9, 4 },   { 3, 4, 2 },
        { 3, 2, 6 },   { 3, 6, 8 }, { 3, 8, 9 },   { 4, 9, 5 },
        { 2, 4, 11 },  { 6, 2, 10 }, { 8, 6, 7 },  { 9, 8, 1 },
    };

    const auto place = [&](const Vec3& unit) {
        const f32 r =
            radius * (1.0f + (directionHash01(seed, unit) - 0.5f) * 2.0f *
                                 jitter);
        return center + unit * r;
    };

    // Recursive subdivision (4 faces per face per level), flat-shaded.
    // Emission order at depth 1 matches the previous hard-coded version,
    // keeping rock/bush meshes bit-identical per seed.
    const auto subdivide = [&](const auto& self, const Vec3& a, const Vec3& b,
                               const Vec3& c, u32 depth) -> void {
        if (depth == 0) {
            appendTriangle(mesh, place(a), place(b), place(c), color);
            return;
        }
        const Vec3 ab = glm::normalize(a + b);
        const Vec3 bc = glm::normalize(b + c);
        const Vec3 ca = glm::normalize(c + a);
        self(self, a, ab, ca, depth - 1);
        self(self, ab, b, bc, depth - 1);
        self(self, ca, bc, c, depth - 1);
        self(self, ab, bc, ca, depth - 1);
    };
    for (const auto& face : kFaces) {
        subdivide(subdivide, glm::normalize(base[face[0]]),
                  glm::normalize(base[face[1]]),
                  glm::normalize(base[face[2]]), subdivisions);
    }
}

void appendBox(MeshData& mesh, const Vec3& center, const Vec3& halfExtents,
               const Vec3& color) {
    // 6 faces × 4 unique vertices, UVs [0,1] per face, outward CCW.
    struct Face {
        Vec3 normal;
        Vec3 tangentU; // maps to u
        Vec3 tangentV; // maps to v
    };
    const Face faces[6] = {
        { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } },
        { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
        { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } },
        { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
        { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },
        { { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } },
    };
    for (const Face& face : faces) {
        const u32 base = static_cast<u32>(mesh.vertices.size());
        for (u32 corner = 0; corner < 4; ++corner) {
            const f32 u = corner == 1 || corner == 2 ? 1.0f : 0.0f;
            const f32 v = corner >= 2 ? 1.0f : 0.0f;
            const Vec3 position =
                center + (face.normal + face.tangentU * (u * 2.0f - 1.0f) +
                          face.tangentV * (v * 2.0f - 1.0f)) *
                             halfExtents;
            mesh.vertices.push_back({ .position = position,
                                      .normal = face.normal,
                                      .uv = { u, v },
                                      .color = color });
        }
        mesh.indices.insert(mesh.indices.end(),
                            { base, base + 1, base + 2, base, base + 2,
                              base + 3 });
    }
}

void recomputeFlatNormals(MeshData& mesh) {
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        MeshVertex& a = mesh.vertices[mesh.indices[i]];
        MeshVertex& b = mesh.vertices[mesh.indices[i + 1]];
        MeshVertex& c = mesh.vertices[mesh.indices[i + 2]];
        const Vec3 normal = glm::normalize(
            glm::cross(b.position - a.position, c.position - a.position));
        a.normal = normal;
        b.normal = normal;
        c.normal = normal;
    }
}

} // namespace render
