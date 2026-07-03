#include "engine/render/MeshBuilder.hpp"

#include <cmath>
#include <unordered_map>

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
                f32 jitter, const Vec3& color) {
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

    // One subdivision (4 faces per face, 80 total), flat-shaded.
    for (const auto& face : kFaces) {
        const Vec3 a = glm::normalize(base[face[0]]);
        const Vec3 b = glm::normalize(base[face[1]]);
        const Vec3 c = glm::normalize(base[face[2]]);
        const Vec3 ab = glm::normalize(a + b);
        const Vec3 bc = glm::normalize(b + c);
        const Vec3 ca = glm::normalize(c + a);
        appendTriangle(mesh, place(a), place(ab), place(ca), color);
        appendTriangle(mesh, place(ab), place(b), place(bc), color);
        appendTriangle(mesh, place(ca), place(bc), place(c), color);
        appendTriangle(mesh, place(ab), place(bc), place(ca), color);
    }
}

} // namespace render
