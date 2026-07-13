#include "game/WeaponMeshes.hpp"

#include <cmath>

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

render::MeshData makeClubMesh(f32 length) {
    const f32 total = glm::max(length, 0.4f);
    const Vec3 kWood { 0.36f, 0.24f, 0.13f };
    const Vec3 kIron { 0.42f, 0.43f, 0.46f };

    render::MeshData mesh;
    // Wooden shaft: from below the fist up into the head (dev design:
    // "un manche et un bout plus grand en métal").
    const f32 headLen = total * 0.32f;
    const f32 shaftTop = total - headLen * 0.6f; // buried in the head
    addBox(mesh, { 0.0f, (shaftTop - 0.14f) * 0.5f, 0.0f },
           { 0.026f, (shaftTop + 0.14f) * 0.5f, 0.026f }, kWood);
    // The metal head: a fatter block at the tip, with a small collar
    // where it meets the wood.
    addBox(mesh, { 0.0f, total - headLen * 0.5f, 0.0f },
           { 0.062f, headLen * 0.5f, 0.062f }, kIron);
    addBox(mesh, { 0.0f, total - headLen - 0.012f, 0.0f },
           { 0.042f, 0.014f, 0.042f }, kIron);
    return mesh;
}

const core::Guid& clubMeshGuid() {
    static const core::Guid guid =
        *core::Guid::fromString("a2b1ade0-0000-4000-8000-00000000501e");
    return guid;
}

render::MeshData makeBowMesh(f32 length) {
    const f32 half = glm::max(length, 0.6f) * 0.5f;
    const Vec3 kWood { 0.32f, 0.21f, 0.11f };
    const Vec3 kString { 0.85f, 0.83f, 0.78f };
    const Vec3 kLeather { 0.34f, 0.22f, 0.12f };

    render::MeshData mesh;
    // Grip block at the origin.
    addBox(mesh, { 0.0f, 0.0f, 0.0f }, { 0.02f, 0.09f, 0.03f }, kLeather);
    // Each limb: segments sweeping up/down with a forward (+Z) belly —
    // a shallow arc of boxes reads as a curve at this poly budget.
    constexpr u32 kSegments = 5;
    for (u32 side = 0; side < 2; ++side) {
        const f32 sign = side == 0 ? 1.0f : -1.0f;
        for (u32 i = 0; i < kSegments; ++i) {
            const f32 t0 = static_cast<f32>(i) / kSegments;
            const f32 t1 = static_cast<f32>(i + 1) / kSegments;
            const f32 midT = (t0 + t1) * 0.5f;
            const f32 y = sign * (0.09f + midT * (half - 0.09f));
            // The belly: strongest mid-limb, back to 0 at the tip.
            const f32 z = 0.10f * std::sin(midT * 3.14159f * 0.85f);
            const f32 thick = glm::mix(0.016f, 0.008f, midT);
            addBox(mesh, { 0.0f, y, z },
                   { thick, (t1 - t0) * (half - 0.09f) * 0.55f,
                     thick * 1.4f },
                   kWood);
        }
    }
    // The string: one thin box tip to tip (behind the grip, z = 0).
    addBox(mesh, { 0.0f, 0.0f, -0.015f }, { 0.004f, half, 0.004f },
           kString);
    return mesh;
}

const core::Guid& bowMeshGuid() {
    static const core::Guid guid =
        *core::Guid::fromString("a2b1ade0-0000-4000-8000-00000000501f");
    return guid;
}

render::MeshData makeArrowMesh(f32 length) {
    const f32 total = glm::max(length, 0.3f);
    const Vec3 kShaft { 0.45f, 0.33f, 0.18f };
    const Vec3 kSteel { 0.62f, 0.64f, 0.68f };
    const Vec3 kFeather { 0.82f, 0.80f, 0.72f };

    render::MeshData mesh;
    const f32 tipLen = 0.06f;
    // Shaft from the nock to the head.
    addBox(mesh, { 0.0f, (total - tipLen) * 0.5f, 0.0f },
           { 0.008f, (total - tipLen) * 0.5f, 0.008f }, kShaft);
    // The head: a small pyramid.
    addTip(mesh, total - tipLen, total, 0.018f, 0.018f, kSteel);
    // Fletching: two crossed vanes near the nock.
    addBox(mesh, { 0.0f, 0.06f, 0.0f }, { 0.030f, 0.045f, 0.003f },
           kFeather);
    addBox(mesh, { 0.0f, 0.06f, 0.0f }, { 0.003f, 0.045f, 0.030f },
           kFeather);
    return mesh;
}

const core::Guid& arrowMeshGuid() {
    static const core::Guid guid =
        *core::Guid::fromString("a2b1ade0-0000-4000-8000-000000005020");
    return guid;
}

render::MeshData makeHorseMesh(f32 shoulderHeight) {
    // Canonical pony: withers (body top) at 1.2 m; everything scales
    // uniformly from there. Feet at y = 0, nose toward +Z.
    const f32 s = glm::max(shoulderHeight, 0.5f) / 1.2f;
    const Vec3 kCoat { 0.45f, 0.30f, 0.18f };    // bay coat
    const Vec3 kDark { 0.20f, 0.13f, 0.09f };    // mane / tail / legs
    const Vec3 kLeather { 0.30f, 0.18f, 0.10f }; // saddle
    const Vec3 kBlanket { 0.55f, 0.20f, 0.16f }; // saddle blanket

    render::MeshData mesh;
    const auto box = [&](const Vec3& center, const Vec3& half,
                         const Vec3& color) {
        addBox(mesh, center * s, half * s, color);
    };
    // Barrel body, top at 1.2 (the withers).
    box({ 0.0f, 0.95f, 0.0f }, { 0.22f, 0.25f, 0.55f }, kCoat);
    // Four legs, ground to belly.
    for (const f32 x : { -0.14f, 0.14f }) {
        for (const f32 z : { -0.38f, 0.38f }) {
            box({ x, 0.36f, z }, { 0.065f, 0.36f, 0.065f }, kDark);
        }
    }
    // Neck rising from the chest, then the head with a muzzle bias
    // forward — enough silhouette to read « poney » at a glance.
    box({ 0.0f, 1.34f, 0.58f }, { 0.10f, 0.26f, 0.13f }, kCoat);
    box({ 0.0f, 1.60f, 0.82f }, { 0.085f, 0.12f, 0.22f }, kCoat);
    // Mane ridge along the neck's back edge + two ear nubs.
    box({ 0.0f, 1.46f, 0.44f }, { 0.03f, 0.20f, 0.06f }, kDark);
    for (const f32 x : { -0.05f, 0.05f }) {
        box({ x, 1.76f, 0.72f }, { 0.02f, 0.06f, 0.03f }, kDark);
    }
    // Tail.
    box({ 0.0f, 0.82f, -0.62f }, { 0.05f, 0.24f, 0.06f }, kDark);
    // Saddle over a blanket, mid-back — where the rider sits.
    box({ 0.0f, 1.22f, -0.05f }, { 0.24f, 0.02f, 0.22f }, kBlanket);
    box({ 0.0f, 1.27f, -0.05f }, { 0.16f, 0.05f, 0.17f }, kLeather);
    return mesh;
}

const core::Guid& horseMeshGuid() {
    static const core::Guid guid =
        *core::Guid::fromString("a2b1ade0-0000-4000-8000-000000005021");
    return guid;
}

} // namespace game
