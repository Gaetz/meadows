#include "game/WeaponMeshes.hpp"

namespace game {

namespace {

// One axis-aligned box with per-face normals and a flat color.
void addBox(render::MeshData& mesh, const Vec3& center, const Vec3& half,
            const Vec3& color) {
    static constexpr Vec3 kNormals[6] = {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
        { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
    };
    for (const Vec3& n : kNormals) {
        // Build a tangent basis for the face.
        const Vec3 u = glm::abs(n.y) > 0.5f ? Vec3 { 1, 0, 0 }
                                            : Vec3 { 0, 1, 0 };
        const Vec3 t = glm::normalize(glm::cross(u, n));
        const Vec3 b = glm::cross(n, t);
        const Vec3 fc = center + n * half; // face center
        const Vec3 th = t * glm::dot(glm::abs(t), half);
        const Vec3 bh = b * glm::dot(glm::abs(b), half);
        const u32 base = static_cast<u32>(mesh.vertices.size());
        mesh.vertices.push_back({ fc - th - bh, n, { 0, 0 }, color });
        mesh.vertices.push_back({ fc + th - bh, n, { 1, 0 }, color });
        mesh.vertices.push_back({ fc + th + bh, n, { 1, 1 }, color });
        mesh.vertices.push_back({ fc - th + bh, n, { 0, 1 }, color });
        mesh.indices.insert(mesh.indices.end(),
                            { base, base + 1, base + 2,
                              base, base + 2, base + 3 });
    }
}

// The tapering tip: a pyramid from the blade's end rectangle to a point.
void addTip(render::MeshData& mesh, f32 baseY, f32 tipY, f32 halfX,
            f32 halfZ, const Vec3& color) {
    const Vec3 apex { 0.0f, tipY, 0.0f };
    const Vec3 corners[4] = {
        { -halfX, baseY, -halfZ },
        { halfX, baseY, -halfZ },
        { halfX, baseY, halfZ },
        { -halfX, baseY, halfZ },
    };
    for (u32 i = 0; i < 4; ++i) {
        const Vec3& a = corners[i];
        const Vec3& b = corners[(i + 1) % 4];
        const Vec3 n =
            glm::normalize(glm::cross(b - a, apex - a));
        const u32 base = static_cast<u32>(mesh.vertices.size());
        mesh.vertices.push_back({ a, n, { 0, 0 }, color });
        mesh.vertices.push_back({ b, n, { 1, 0 }, color });
        mesh.vertices.push_back({ apex, n, { 0.5f, 1 }, color });
        mesh.indices.insert(mesh.indices.end(),
                            { base, base + 1, base + 2 });
    }
}

} // namespace

render::MeshData makeSwordMesh(f32 bladeLength) {
    const f32 length = glm::max(bladeLength, 0.4f);
    const Vec3 kSteel { 0.72f, 0.74f, 0.78f };
    const Vec3 kDark { 0.16f, 0.14f, 0.12f };
    const Vec3 kLeather { 0.34f, 0.22f, 0.12f };

    render::MeshData mesh;
    // Grip: below the origin (the hand holds the origin), pommel at the end.
    addBox(mesh, { 0.0f, -0.075f, 0.0f }, { 0.022f, 0.075f, 0.022f },
           kLeather);
    addBox(mesh, { 0.0f, -0.165f, 0.0f }, { 0.032f, 0.018f, 0.032f },
           kDark);
    // Cross guard.
    addBox(mesh, { 0.0f, 0.02f, 0.0f }, { 0.095f, 0.014f, 0.022f }, kDark);
    // Blade body up to the tip taper.
    const f32 tipLen = glm::min(0.14f, length * 0.2f);
    const f32 bodyTop = length - tipLen;
    addBox(mesh, { 0.0f, (0.034f + bodyTop) * 0.5f, 0.0f },
           { 0.028f, (bodyTop - 0.034f) * 0.5f, 0.007f }, kSteel);
    addTip(mesh, bodyTop, length, 0.028f, 0.007f, kSteel);
    return mesh;
}

const core::Guid& swordMeshGuid() {
    static const core::Guid guid =
        *core::Guid::fromString("a2b1ade0-0000-4000-8000-00000000501d");
    return guid;
}

} // namespace game
