#include "engine/terrain/generation/Authoring.hpp"

#include <cmath>

#include <glm/glm.hpp>

#include "engine/core/Bezier.hpp"
#include "engine/terrain/Noise.hpp"

namespace render::terraingen {

namespace {

// Rim falloff shared by the stamps: 1 inside the hard core, smoothstep
// to 0 at the radius.
f32 rimFalloff(f32 dist, f32 radius, f32 hardness) {
    const f32 core = glm::clamp(hardness, 0.0f, 0.95f) * radius;
    if (dist <= core) {
        return 1.0f;
    }
    return 1.0f - noise::smoothstep01(core, radius, dist);
}

} // namespace

void stampKernel(const GridSpec& spec, vector<f32>& height, Vec2 center,
                 f32 radius, f32 amplitude, StampMode mode,
                 f32 hardness) {
    if (radius <= 0.0f) {
        return;
    }
    const i32 n = static_cast<i32>(spec.n);
    const auto clampIndex = [&](f32 v) {
        return glm::clamp(static_cast<i32>(std::floor(v)), 0, n - 1);
    };
    const i32 c0 = clampIndex((center.x - radius - spec.originX) /
                              spec.texelSize);
    const i32 c1 = clampIndex((center.x + radius - spec.originX) /
                              spec.texelSize);
    const i32 r0 = clampIndex((center.y - radius - spec.originZ) /
                              spec.texelSize);
    const i32 r1 = clampIndex((center.y + radius - spec.originZ) /
                              spec.texelSize);
    for (i32 row = r0; row <= r1; ++row) {
        for (i32 col = c0; col <= c1; ++col) {
            const f32 x = spec.x(static_cast<u32>(col));
            const f32 z = spec.z(static_cast<u32>(row));
            const f32 dist = std::hypot(x - center.x, z - center.y);
            if (dist >= radius) {
                continue;
            }
            const f32 w = rimFalloff(dist, radius, hardness);
            f32& h = height[static_cast<size_t>(row) * spec.n + col];
            switch (mode) {
            case StampMode::Add:
                h += amplitude * w;
                break;
            case StampMode::Max:
                h = glm::max(h, amplitude * w);
                break;
            case StampMode::Blend:
                h = glm::mix(h, amplitude, w);
                break;
            }
        }
    }
}

void stampRidge(const GridSpec& spec, vector<f32>& height,
                const RidgeStroke& stroke) {
    // Sample the Bezier densely relative to the texel size, then treat
    // the samples as a polyline: distance to it = distance to the
    // crest. Max-composed so crossing strokes merge.
    const f32 approxLen = glm::length(stroke.p1 - stroke.p0) +
                          glm::length(stroke.p2 - stroke.p1) +
                          glm::length(stroke.p3 - stroke.p2);
    const u32 samples = glm::clamp(
        static_cast<u32>(approxLen / (spec.texelSize * 0.5f)), 8u,
        4096u);
    vector<Vec3> crest;
    crest.reserve(samples + 1);
    for (u32 k = 0; k <= samples; ++k) {
        crest.push_back(core::bezierPoint(
            stroke.p0, stroke.p1, stroke.p2, stroke.p3,
            static_cast<f32>(k) / static_cast<f32>(samples)));
    }
    const f32 reach = stroke.crestWidth * 0.5f + stroke.falloff;
    for (size_t s = 0; s + 1 < crest.size(); ++s) {
        const Vec3& a = crest[s];
        const Vec3& b = crest[s + 1];
        const i32 n = static_cast<i32>(spec.n);
        const auto clampIndex = [&](f32 v) {
            return glm::clamp(static_cast<i32>(std::floor(v)), 0, n - 1);
        };
        const i32 c0 = clampIndex(
            (glm::min(a.x, b.x) - reach - spec.originX) / spec.texelSize);
        const i32 c1 = clampIndex(
            (glm::max(a.x, b.x) + reach - spec.originX) / spec.texelSize);
        const i32 r0 = clampIndex(
            (glm::min(a.z, b.z) - reach - spec.originZ) / spec.texelSize);
        const i32 r1 = clampIndex(
            (glm::max(a.z, b.z) + reach - spec.originZ) / spec.texelSize);
        const f32 abx = b.x - a.x;
        const f32 abz = b.z - a.z;
        const f32 len2 = abx * abx + abz * abz;
        for (i32 row = r0; row <= r1; ++row) {
            for (i32 col = c0; col <= c1; ++col) {
                const f32 x = spec.x(static_cast<u32>(col));
                const f32 z = spec.z(static_cast<u32>(row));
                const f32 t =
                    len2 > 0.0f
                        ? glm::clamp(((x - a.x) * abx + (z - a.z) * abz) /
                                         len2,
                                     0.0f, 1.0f)
                        : 0.0f;
                const f32 px = a.x + abx * t;
                const f32 pz = a.z + abz * t;
                const f32 dist = std::hypot(x - px, z - pz);
                if (dist >= reach) {
                    continue;
                }
                const f32 crestY = glm::mix(a.y, b.y, t);
                const f32 side = glm::max(
                    dist - stroke.crestWidth * 0.5f, 0.0f);
                const f32 w =
                    1.0f - noise::smoothstep01(0.0f, stroke.falloff,
                                               side);
                const size_t i =
                    static_cast<size_t>(row) * spec.n + col;
                // Max-composition against the EXISTING ground: the
                // ridge grows out of the terrain, never trenches it.
                height[i] = glm::max(
                    height[i], glm::mix(height[i], crestY, w));
            }
        }
    }
}

f32 baseElevationAt(std::span<const ElevationAnchor> anchors, f32 x,
                    f32 z, f32 background) {
    f32 weightSum = 0.0f;
    f32 sum = 0.0f;
    f32 nearest = 1.0f; // 1 - w of the closest anchor
    for (const ElevationAnchor& anchor : anchors) {
        const f32 dist = std::hypot(x - anchor.x, z - anchor.z);
        if (dist >= anchor.radius) {
            continue;
        }
        const f32 w =
            1.0f - noise::smoothstep01(0.0f, anchor.radius, dist);
        sum += anchor.elevation * w;
        weightSum += w;
        nearest = glm::min(nearest, 1.0f - w);
    }
    if (weightSum <= 0.0f) {
        return background;
    }
    // Normalized blend of the anchors, faded back to the background at
    // the reach of the nearest one.
    return glm::mix(sum / weightSum, background, nearest);
}

void alterElevation(const GridSpec& spec, vector<f32>& height,
                    Vec2 center, f32 radius, f32 target,
                    f32 blendExponent) {
    if (radius <= 0.0f) {
        return;
    }
    // The delta is measured ONCE at the center and applied weighted:
    // relief inside the disc survives, only the base shifts.
    const i32 cc = glm::clamp(
        static_cast<i32>(std::lround((center.x - spec.originX) /
                                     spec.texelSize)),
        0, static_cast<i32>(spec.n) - 1);
    const i32 cr = glm::clamp(
        static_cast<i32>(std::lround((center.y - spec.originZ) /
                                     spec.texelSize)),
        0, static_cast<i32>(spec.n) - 1);
    const f32 delta =
        target - height[static_cast<size_t>(cr) * spec.n + cc];
    const i32 n = static_cast<i32>(spec.n);
    const auto clampIndex = [&](f32 v) {
        return glm::clamp(static_cast<i32>(std::floor(v)), 0, n - 1);
    };
    const i32 c0 = clampIndex((center.x - radius - spec.originX) /
                              spec.texelSize);
    const i32 c1 = clampIndex((center.x + radius - spec.originX) /
                              spec.texelSize);
    const i32 r0 = clampIndex((center.y - radius - spec.originZ) /
                              spec.texelSize);
    const i32 r1 = clampIndex((center.y + radius - spec.originZ) /
                              spec.texelSize);
    for (i32 row = r0; row <= r1; ++row) {
        for (i32 col = c0; col <= c1; ++col) {
            const f32 x = spec.x(static_cast<u32>(col));
            const f32 z = spec.z(static_cast<u32>(row));
            const f32 dist = std::hypot(x - center.x, z - center.y);
            if (dist >= radius) {
                continue;
            }
            const f32 w = std::pow(
                1.0f - noise::smoothstep01(0.0f, radius, dist),
                blendExponent);
            height[static_cast<size_t>(row) * spec.n + col] +=
                delta * w;
        }
    }
}

} // namespace render::terraingen
