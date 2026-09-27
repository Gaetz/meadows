#include "world/spirit/SpiritWaterMesh.hpp"

#include <cmath>

#include <glm/glm.hpp>

namespace world {

namespace {

constexpr f32 kTwoPi = 6.2831853f;

} // namespace

f32 jetRadius(f32 intensity, f32 speed) {
    const f32 area = glm::max(intensity, 0.0f) / glm::max(speed, 0.1f);
    return glm::clamp(std::sqrt(area / 3.1415927f), 0.08f, 0.6f);
}

f32 blobRadius(f32 volume) {
    const f32 r = std::cbrt(3.0f * glm::max(volume, 0.0f) / (4.0f * 3.1415927f));
    return glm::clamp(r, 0.25f, 3.0f);
}

void appendJetTube(vector<Vec3>& out, const Vec3& origin,
                   const Vec3& velocity, f32 gravity, f32 seconds,
                   f32 radius, u32 segments, u32 sides, f32 flare) {
    if (segments < 1 || sides < 3 || seconds <= 0.0f || radius <= 0.0f) {
        return;
    }
    const auto at = [&](f32 t) {
        return Vec3 { origin.x + velocity.x * t,
                      origin.y + velocity.y * t - 0.5f * gravity * t * t,
                      origin.z + velocity.z * t };
    };
    const auto tangentAt = [&](f32 t) {
        const Vec3 v { velocity.x, velocity.y - gravity * t, velocity.z };
        const f32 len = glm::length(v);
        return len > 1e-5f ? v / len : Vec3 { 0.0f, -1.0f, 0.0f };
    };
    // One ring per sample: a frame from the tangent and a stable side
    // vector (world up unless the tangent is vertical).
    vector<Vec3> rings;
    rings.reserve(static_cast<size_t>(segments + 1) * sides);
    for (u32 i = 0; i <= segments; ++i) {
        const f32 u = static_cast<f32>(i) / static_cast<f32>(segments);
        const f32 t = u * seconds;
        const Vec3 c = at(t);
        const Vec3 tangent = tangentAt(t);
        Vec3 up { 0.0f, 1.0f, 0.0f };
        if (std::abs(glm::dot(tangent, up)) > 0.95f) {
            up = { 1.0f, 0.0f, 0.0f };
        }
        const Vec3 side = glm::normalize(glm::cross(tangent, up));
        const Vec3 norm = glm::cross(side, tangent);
        const f32 r = radius * (1.0f + (flare - 1.0f) * u);
        for (u32 k = 0; k < sides; ++k) {
            const f32 a = kTwoPi * static_cast<f32>(k) / static_cast<f32>(sides);
            rings.push_back(c + (side * std::cos(a) + norm * std::sin(a)) * r);
        }
    }
    const auto ring = [&](u32 i, u32 k) -> const Vec3& {
        return rings[static_cast<size_t>(i) * sides + (k % sides)];
    };
    for (u32 i = 0; i < segments; ++i) {
        for (u32 k = 0; k < sides; ++k) {
            const Vec3& a = ring(i, k);
            const Vec3& b = ring(i, k + 1);
            const Vec3& c = ring(i + 1, k + 1);
            const Vec3& d = ring(i + 1, k);
            out.push_back(a);
            out.push_back(b);
            out.push_back(c);
            out.push_back(a);
            out.push_back(c);
            out.push_back(d);
        }
    }
}

void appendBlob(vector<Vec3>& out, const Vec3& center, f32 radius,
                u32 rings, u32 sectors, f32 squash) {
    if (rings < 2 || sectors < 3 || radius <= 0.0f) {
        return;
    }
    const auto point = [&](u32 i, u32 k) {
        const f32 v = 3.1415927f * static_cast<f32>(i) / static_cast<f32>(rings);
        const f32 u = kTwoPi * static_cast<f32>(k % sectors) /
                      static_cast<f32>(sectors);
        return center + Vec3 { std::sin(v) * std::cos(u) * radius,
                               std::cos(v) * radius * squash,
                               std::sin(v) * std::sin(u) * radius };
    };
    for (u32 i = 0; i < rings; ++i) {
        for (u32 k = 0; k < sectors; ++k) {
            const Vec3 a = point(i, k);
            const Vec3 b = point(i, k + 1);
            const Vec3 c = point(i + 1, k + 1);
            const Vec3 d = point(i + 1, k);
            if (i > 0) {
                out.push_back(a);
                out.push_back(b);
                out.push_back(c);
            }
            if (i + 1 < rings) {
                out.push_back(a);
                out.push_back(c);
                out.push_back(d);
            }
        }
    }
}

} // namespace world
