#pragma once

#include <cmath>

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

// Baked-terrain base data. Headless data structs shared by the terrain
// height code (engine/render/landscape/TerrainNoise) and the world layer
// (world/terrain/TerrainRegions) that builds them from TerrainRegionForm
// records + `.trg` assets — same split as HeightPatches, and the same
// reason: world/ must not depend on engine/render/ (§2.10). Namespace kept
// as `render` for the same no-churn rationale as HeightPatches.
//
// A region is an ABSOLUTE height grid (meters) replacing the procedural
// base inside its rectangle; outside, the procedural base continues and
// the two blend over `edgeBlend` meters inside the rim. Sculpt deltas
// (HeightPatches) stay a separate layer applied on top — the three layers
// are never flattened together.

namespace render {

struct TerrainRegion {
    f32 originX { 0.0f }; // world meters, min corner of the covered rect
    f32 originZ { 0.0f };
    f32 texelSize { 2.0f };   // meters per height texel
    f32 edgeBlend { 256.0f }; // meters of procedural<->baked blend inside
                              // the rim; 0 = hard edge (tests only)
    u32 width { 0 };  // height samples per row (x fastest)
    u32 height { 0 }; // rows along +Z
    vector<f32> heights; // meters, width * height, row-major

    // Coarse mask channels, all maskWidth x maskHeight (typically wider
    // texels than `heights`); empty until the generator fills them.
    // detailAmp scales the runtime detail noise; the others feed splat,
    // scatter and water systems.
    u32 maskWidth { 0 };
    u32 maskHeight { 0 };
    vector<u8> detailAmp; // 0..255 -> 0..1 multiplier
    vector<u8> flow;      // log-scaled drainage
    vector<u8> wetness;
    vector<u8> beach;
    vector<u8> biome; // biome palette index, nearest-sampled

    // Runtime detail-noise character inside this region (from
    // TerrainRegionForm). Zero amplitude = the grid is exact (debug bakes
    // of the procedural terrain, which already contains its own detail).
    f32 detailAmplitude { 0.0f };
    f32 detailWavelength { 60.0f };
    i32 detailOctaves { 3 };

    f32 spanX() const { return static_cast<f32>(width - 1) * texelSize; }
    f32 spanZ() const { return static_cast<f32>(height - 1) * texelSize; }

    bool contains(f32 x, f32 z) const {
        return width >= 2 && height >= 2 && x >= originX && z >= originZ &&
               x <= originX + spanX() && z <= originZ + spanZ();
    }
};

struct TerrainBase {
    vector<TerrainRegion> regions; // few; linear scan, first hit wins

    const TerrainRegion* regionAt(f32 x, f32 z) const {
        for (const TerrainRegion& region : regions) {
            if (region.contains(x, z)) {
                return &region;
            }
        }
        return nullptr;
    }
};

namespace terrain {

inline f32 catmullRom(f32 p0, f32 p1, f32 p2, f32 p3, f32 t) {
    return p1 +
           0.5f * t *
               (p2 - p0 +
                t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 +
                     t * (3.0f * (p1 - p2) + p3 - p0)));
}

// Bicubic Catmull-Rom sample of the region's height grid, edge-clamped.
// Bicubic (not bilinear) is load-bearing: central-difference normals over
// a bilinear surface facet at every texel of the macro grid. Catmull-Rom
// passes through the samples and is C1 — enough, the detail noise on top
// masks the residual.
inline f32 baseHeight(const TerrainRegion& r, f32 x, f32 z) {
    const f32 u = (x - r.originX) / r.texelSize;
    const f32 v = (z - r.originZ) / r.texelSize;
    const f32 fu = std::floor(u);
    const f32 fv = std::floor(v);
    const i32 iu = static_cast<i32>(fu);
    const i32 iv = static_cast<i32>(fv);
    const f32 tu = u - fu;
    const f32 tv = v - fv;
    const auto at = [&](i32 cx, i32 cz) {
        cx = glm::clamp(cx, 0, static_cast<i32>(r.width) - 1);
        cz = glm::clamp(cz, 0, static_cast<i32>(r.height) - 1);
        return r.heights[static_cast<size_t>(cz) * r.width +
                         static_cast<size_t>(cx)];
    };
    f32 rows[4];
    for (i32 j = 0; j < 4; ++j) {
        const i32 cz = iv - 1 + j;
        rows[j] = catmullRom(at(iu - 1, cz), at(iu, cz), at(iu + 1, cz),
                             at(iu + 2, cz), tu);
    }
    return catmullRom(rows[0], rows[1], rows[2], rows[3], tv);
}

// 1 at edgeBlend meters (or more) inside the region rect, 0 on the rim —
// smoothstep between. mix(procedural, baked, edgeWeight) is exact at both
// ends, so the border is seam-free by construction.
inline f32 edgeWeight(const TerrainRegion& r, f32 x, f32 z) {
    const f32 dx = glm::min(x - r.originX, r.originX + r.spanX() - x);
    const f32 dz = glm::min(z - r.originZ, r.originZ + r.spanZ() - z);
    const f32 d = glm::min(dx, dz);
    if (r.edgeBlend <= 0.0f) {
        return d >= 0.0f ? 1.0f : 0.0f;
    }
    const f32 t = glm::clamp(d / r.edgeBlend, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Bilinear sample of a u8 mask channel in [0, 1]; `fallback` where the
// channel is not baked.
inline f32 maskSample(const TerrainRegion& r, const vector<u8>& channel,
                      f32 x, f32 z, f32 fallback) {
    if (channel.empty() || r.maskWidth < 2 || r.maskHeight < 2) {
        return fallback;
    }
    const f32 texel = r.spanX() / static_cast<f32>(r.maskWidth - 1);
    const f32 u = glm::clamp((x - r.originX) / texel, 0.0f,
                             static_cast<f32>(r.maskWidth - 1));
    const f32 texelZ = r.spanZ() / static_cast<f32>(r.maskHeight - 1);
    const f32 v = glm::clamp((z - r.originZ) / texelZ, 0.0f,
                             static_cast<f32>(r.maskHeight - 1));
    const u32 u0 = glm::min(static_cast<u32>(u), r.maskWidth - 2);
    const u32 v0 = glm::min(static_cast<u32>(v), r.maskHeight - 2);
    const f32 tu = u - static_cast<f32>(u0);
    const f32 tv = v - static_cast<f32>(v0);
    const auto at = [&](u32 cx, u32 cz) {
        return static_cast<f32>(channel[static_cast<size_t>(cz) *
                                            r.maskWidth +
                                        cx]) *
               (1.0f / 255.0f);
    };
    const f32 a = at(u0, v0) + (at(u0 + 1, v0) - at(u0, v0)) * tu;
    const f32 b =
        at(u0, v0 + 1) + (at(u0 + 1, v0 + 1) - at(u0, v0 + 1)) * tu;
    return a + (b - a) * tv;
}

} // namespace terrain

} // namespace render
