#include "engine/terrain/generation/MapExport.hpp"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

#include "engine/terrain/generation/MasterNetwork.hpp"

namespace render::terraingen {

namespace {

struct Rgb {
    f32 r, g, b;
};

Rgb mix(const Rgb& a, const Rgb& b, f32 t) {
    return { glm::mix(a.r, b.r, t), glm::mix(a.g, b.g, t),
             glm::mix(a.b, b.b, t) };
}

// Hypsometric palette (usual soil colors; land keyed on height above
// sea, snow at the sandbox snow line).
Rgb landColor(f32 rel) {
    static const struct {
        f32 h;
        Rgb c;
    } kStops[] = {
        { 2.0f, { 0.44f, 0.63f, 0.38f } },    // lowland green
        { 120.0f, { 0.52f, 0.66f, 0.36f } },  // plains
        { 300.0f, { 0.62f, 0.62f, 0.36f } },  // hills olive
        { 550.0f, { 0.65f, 0.54f, 0.37f } },  // highland tan
        { 850.0f, { 0.54f, 0.47f, 0.40f } },  // mountain brown
        { 1050.0f, { 0.60f, 0.60f, 0.60f } }, // rock grey
        { 1150.0f, { 0.95f, 0.95f, 0.97f } }, // snow
    };
    if (rel <= kStops[0].h) {
        return kStops[0].c;
    }
    for (size_t i = 1; i < std::size(kStops); ++i) {
        if (rel <= kStops[i].h) {
            const f32 t = (rel - kStops[i - 1].h) /
                          (kStops[i].h - kStops[i - 1].h);
            return mix(kStops[i - 1].c, kStops[i].c, t);
        }
    }
    return kStops[std::size(kStops) - 1].c;
}

} // namespace

vector<u8> renderTerrainMap(const ProceduralControls& controls,
                            const MacroParams& macro,
                            const TerrainMapParams& params) {
    const u32 n = glm::max(params.size, 16u);
    const f32 texel = params.span / static_cast<f32>(n);
    const f32 minX = params.centerX - params.span * 0.5f;
    const f32 minZ = params.centerZ - params.span * 0.5f;
    const f32 sea = macro.seaLevel;

    vector<f32> height(static_cast<size_t>(n) * n);
    for (u32 row = 0; row < n; ++row) {
        for (u32 col = 0; col < n; ++col) {
            height[static_cast<size_t>(row) * n + col] =
                macroHeightAnalytic(controls, macro,
                                    minX + static_cast<f32>(col) * texel,
                                    minZ + static_cast<f32>(row) * texel);
        }
    }

    vector<u8> out(static_cast<size_t>(n) * n * 3);
    const auto at = [&](i32 col, i32 row) {
        col = glm::clamp(col, 0, static_cast<i32>(n) - 1);
        row = glm::clamp(row, 0, static_cast<i32>(n) - 1);
        return height[static_cast<size_t>(row) * n + col];
    };
    for (u32 row = 0; row < n; ++row) {
        for (u32 col = 0; col < n; ++col) {
            const f32 h = at(static_cast<i32>(col),
                             static_cast<i32>(row));
            Rgb c;
            if (h <= sea) {
                // Sea: depth gradient, shallow shelf readable.
                const f32 depth = glm::clamp((sea - h) / 90.0f, 0.0f,
                                             1.0f);
                c = mix({ 0.55f, 0.72f, 0.86f }, { 0.07f, 0.19f, 0.38f },
                        std::sqrt(depth));
            } else if (h <= sea + 2.0f) {
                c = { 0.85f, 0.77f, 0.54f }; // sand line
            } else {
                c = landColor(h - sea);
                // Hillshade (light from the north-west).
                const f32 sx = (at(static_cast<i32>(col) + 1,
                                   static_cast<i32>(row)) -
                                at(static_cast<i32>(col) - 1,
                                   static_cast<i32>(row))) /
                               (2.0f * texel);
                const f32 sz = (at(static_cast<i32>(col),
                                   static_cast<i32>(row) + 1) -
                                at(static_cast<i32>(col),
                                   static_cast<i32>(row) - 1)) /
                               (2.0f * texel);
                const f32 shade = glm::clamp(
                    0.72f - 0.9f * (sx + sz) /
                                std::sqrt(1.0f + sx * sx + sz * sz),
                    0.35f, 1.15f);
                c = { c.r * shade, c.g * shade, c.b * shade };
            }
            u8* px = &out[(static_cast<size_t>(row) * n + col) * 3];
            px[0] = static_cast<u8>(glm::clamp(c.r, 0.0f, 1.0f) * 255.0f);
            px[1] = static_cast<u8>(glm::clamp(c.g, 0.0f, 1.0f) * 255.0f);
            px[2] = static_cast<u8>(glm::clamp(c.b, 0.0f, 1.0f) * 255.0f);
        }
    }

    const auto plot = [&](i32 col, i32 row, const Rgb& c) {
        if (col < 0 || row < 0 || col >= static_cast<i32>(n) ||
            row >= static_cast<i32>(n)) {
            return;
        }
        u8* px = &out[(static_cast<size_t>(row) * n + col) * 3];
        px[0] = static_cast<u8>(c.r * 255.0f);
        px[1] = static_cast<u8>(c.g * 255.0f);
        px[2] = static_cast<u8>(c.b * 255.0f);
    };

    // Beyond ~200 m/px the courses are sub-pixel and the master-network
    // sweep (one routing per 24 km super cell) dominates the runtime —
    // continent-scale maps skip them.
    if (params.drawRivers && texel <= 200.0f) {
        MasterNetworkParams network;
        network.seaLevel = sea;
        const auto rivers = masterRiversNear(
            controls, macro, network, minX, minZ, minX + params.span,
            minZ + params.span);
        const Rgb riverBlue { 0.16f, 0.38f, 0.72f };
        for (const MasterRiver& river : rivers) {
            for (size_t k = 1; k < river.nodes.size(); ++k) {
                const MasterNode& a = river.nodes[k - 1];
                const MasterNode& b = river.nodes[k];
                const f32 length = std::hypot(b.x - a.x, b.z - a.z);
                const i32 steps = glm::max(
                    2, static_cast<i32>(length / (texel * 0.5f)));
                // Wider stroke for bigger drainage.
                const i32 thick = b.area > 2.0e7f ? 1 : 0;
                for (i32 s = 0; s <= steps; ++s) {
                    const f32 t = static_cast<f32>(s) /
                                  static_cast<f32>(steps);
                    const i32 col = static_cast<i32>(
                        (glm::mix(a.x, b.x, t) - minX) / texel);
                    const i32 row = static_cast<i32>(
                        (glm::mix(a.z, b.z, t) - minZ) / texel);
                    for (i32 dz = -thick; dz <= thick; ++dz) {
                        for (i32 dx = -thick; dx <= thick; ++dx) {
                            plot(col + dx, row + dz, riverBlue);
                        }
                    }
                }
            }
        }
    }

    if (params.markCenter) {
        const Rgb red { 0.85f, 0.15f, 0.15f };
        const i32 cc = static_cast<i32>(n) / 2;
        for (i32 d = -6; d <= 6; ++d) {
            plot(cc + d, cc, red);
            plot(cc, cc + d, red);
        }
    }
    return out;
}

vector<u8> renderRegionRelief(const TerrainRegion& region, f32 seaLevel,
                              u32 size, f32 cropCenterX,
                              f32 cropCenterZ, f32 cropSpan) {
    const u32 n = glm::max(size, 16u);
    vector<u8> out(static_cast<size_t>(n) * n * 3);
    // Crop window in region-normalized [0,1] uv (full region if 0).
    f32 u0 = 0.0f, v0 = 0.0f, uw = 1.0f, vw = 1.0f;
    if (cropSpan > 0.0f) {
        u0 = glm::clamp(
            (cropCenterX - cropSpan * 0.5f - region.originX) /
                region.spanX(),
            0.0f, 1.0f);
        v0 = glm::clamp(
            (cropCenterZ - cropSpan * 0.5f - region.originZ) /
                region.spanZ(),
            0.0f, 1.0f);
        uw = glm::min(cropSpan / region.spanX(), 1.0f - u0);
        vw = glm::min(cropSpan / region.spanZ(), 1.0f - v0);
    }
    const auto sample = [&](f32 u, f32 v) {
        u = u0 + u * uw;
        v = v0 + v * vw;
        const f32 col = u * static_cast<f32>(region.width - 1);
        const f32 row = v * static_cast<f32>(region.height - 1);
        const u32 c0 = static_cast<u32>(col);
        const u32 r0 = static_cast<u32>(row);
        const u32 c1 = glm::min(c0 + 1, region.width - 1);
        const u32 r1 = glm::min(r0 + 1, region.height - 1);
        const f32 tc = col - static_cast<f32>(c0);
        const f32 tr = row - static_cast<f32>(r0);
        const auto at = [&](u32 c, u32 r) {
            return region
                .heights[static_cast<size_t>(r) * region.width + c];
        };
        return glm::mix(glm::mix(at(c0, r0), at(c1, r0), tc),
                        glm::mix(at(c0, r1), at(c1, r1), tc), tr);
    };
    const f32 metersPerPx =
        region.spanX() * uw / static_cast<f32>(n);
    for (u32 row = 0; row < n; ++row) {
        for (u32 col = 0; col < n; ++col) {
            const f32 u = static_cast<f32>(col) / static_cast<f32>(n - 1);
            const f32 v = static_cast<f32>(row) / static_cast<f32>(n - 1);
            const f32 h = sample(u, v);
            Rgb c;
            if (h <= seaLevel) {
                const f32 depth =
                    glm::clamp((seaLevel - h) / 40.0f, 0.0f, 1.0f);
                c = mix({ 0.55f, 0.72f, 0.86f },
                        { 0.07f, 0.19f, 0.38f }, std::sqrt(depth));
            } else {
                c = landColor(h - seaLevel);
                const f32 e = 1.0f / static_cast<f32>(n - 1);
                const f32 sx =
                    (sample(glm::min(u + e, 1.0f), v) -
                     sample(glm::max(u - e, 0.0f), v)) /
                    (2.0f * metersPerPx);
                const f32 sz =
                    (sample(u, glm::min(v + e, 1.0f)) -
                     sample(u, glm::max(v - e, 0.0f))) /
                    (2.0f * metersPerPx);
                // Normalized Lambert (NW light, ambient floor): reads
                // carve detail without saturating steep faces to
                // black the way the cheap overview shading does.
                const f32 inv =
                    1.0f / std::sqrt(1.0f + sx * sx + sz * sz);
                const f32 lambert = glm::max(
                    (0.62f - 0.55f * sx - 0.55f * sz) * inv, 0.0f);
                const f32 shade = 0.3f + 0.85f * lambert;
                c = { c.r * shade, c.g * shade, c.b * shade };
            }
            u8* px = &out[(static_cast<size_t>(row) * n + col) * 3];
            px[0] = static_cast<u8>(glm::clamp(c.r, 0.0f, 1.0f) * 255.0f);
            px[1] = static_cast<u8>(glm::clamp(c.g, 0.0f, 1.0f) * 255.0f);
            px[2] = static_cast<u8>(glm::clamp(c.b, 0.0f, 1.0f) * 255.0f);
        }
    }
    return out;
}

} // namespace render::terraingen
