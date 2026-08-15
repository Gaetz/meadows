#include "engine/terrain/WaterInfoMap.hpp"

#include <cmath>

#include <glm/glm.hpp>

#include "engine/terrain/RiverGeometry.hpp"

namespace render::terrain {

WaterInfoMap bakeWaterInfo(const WaterBodies& bodies, Vec2 center,
                           f32 span, u32 size, const HeightFn& height) {
    WaterInfoMap map;
    map.span = span;
    map.size = size;
    const f32 texel = span / static_cast<f32>(size);
    map.center = glm::floor(center / texel) * texel;
    const size_t cells = static_cast<size_t>(size) * size;
    map.surface.assign(cells, kWaterInfoDry);
    map.depth.assign(cells, 0.0f);
    map.flow.assign(cells, Vec2 { 0.0f, 0.0f });

    const f32 minX = map.center.x - span * 0.5f;
    const f32 minZ = map.center.y - span * 0.5f;
    const auto clampIndex = [&](f32 v) {
        return glm::clamp(static_cast<i32>(std::floor(v)), 0,
                          static_cast<i32>(size) - 1);
    };
    const auto worldX = [&](i32 col) {
        return minX + (static_cast<f32>(col) + 0.5f) * texel;
    };
    const auto worldZ = [&](i32 row) {
        return minZ + (static_cast<f32>(row) + 0.5f) * texel;
    };

    // Lakes: iterate the map texels inside each lake's bbox, gated by
    // the basin mask — surfaces stack by max (a pond under a lake never
    // wins).
    for (const LakeSurface& lake : bodies.lakes) {
        const i32 c0 = clampIndex((lake.minX - minX) / texel);
        const i32 c1 = clampIndex((lake.maxX - minX) / texel);
        const i32 r0 = clampIndex((lake.minZ - minZ) / texel);
        const i32 r1 = clampIndex((lake.maxZ - minZ) / texel);
        for (i32 row = r0; row <= r1; ++row) {
            for (i32 col = c0; col <= c1; ++col) {
                const f32 x = worldX(col);
                const f32 z = worldZ(row);
                if (!lake.covers(x, z)) {
                    continue;
                }
                const size_t i = static_cast<size_t>(row) * size + col;
                map.surface[i] = glm::max(map.surface[i], lake.level);
            }
        }
    }

    // Rivers: rasterize each (subdivided) segment's footprint. Within
    // ONE river a texel keeps its nearest-lateral segment (the
    // riverFlowSample rule); ACROSS rivers contributions blend by bank
    // weight — the per-pixel junction resolution.
    // Allocated only when rivers exist (sea + lakes alone skip ~5 full-map
    // arrays); empty vectors mean "no river claimed anything" below.
    vector<f32> sumWeight;
    vector<Vec2> sumFlow;
    vector<f32> bestLat;
    vector<Vec2> riverFlow;
    vector<f32> riverLevel;
    vector<u32> touched;
    if (!bodies.rivers.empty()) {
        sumWeight.assign(cells, 0.0f);
        sumFlow.assign(cells, Vec2 { 0.0f, 0.0f });
        bestLat.assign(cells, 2.0f);
        riverFlow.resize(cells);
        riverLevel.resize(cells);
    }
    for (const RiverSurface& river : bodies.rivers) {
        touched.clear();
        const vector<RiverNode> nodes =
            subdivideRiverNodes(river.nodes);
        for (size_t s = 0; s + 1 < nodes.size(); ++s) {
            const RiverNode& a = nodes[s];
            const RiverNode& b = nodes[s + 1];
            const f32 abx = b.x - a.x;
            const f32 abz = b.z - a.z;
            const f32 len2 = abx * abx + abz * abz;
            if (len2 <= 0.0f) {
                continue;
            }
            const f32 reach = glm::max(a.halfWidth, b.halfWidth) + texel;
            const i32 c0 =
                clampIndex((glm::min(a.x, b.x) - reach - minX) / texel);
            const i32 c1 =
                clampIndex((glm::max(a.x, b.x) + reach - minX) / texel);
            const i32 r0 =
                clampIndex((glm::min(a.z, b.z) - reach - minZ) / texel);
            const i32 r1 =
                clampIndex((glm::max(a.z, b.z) + reach - minZ) / texel);
            const f32 len = std::sqrt(len2);
            const Vec2 dir { abx / len, abz / len };
            for (i32 row = r0; row <= r1; ++row) {
                for (i32 col = c0; col <= c1; ++col) {
                    const f32 x = worldX(col);
                    const f32 z = worldZ(row);
                    const f32 t = glm::clamp(
                        ((x - a.x) * abx + (z - a.z) * abz) / len2, 0.0f,
                        1.0f);
                    const f32 px = a.x + abx * t;
                    const f32 pz = a.z + abz * t;
                    const f32 dist = std::sqrt((x - px) * (x - px) +
                                               (z - pz) * (z - pz));
                    const f32 half =
                        glm::mix(a.halfWidth, b.halfWidth, t);
                    if (half <= 0.0f || dist > half) {
                        continue;
                    }
                    const f32 lat = dist / half;
                    const size_t i =
                        static_cast<size_t>(row) * size + col;
                    if (lat >= bestLat[i]) {
                        continue;
                    }
                    if (bestLat[i] > 1.0f) {
                        touched.push_back(static_cast<u32>(i));
                    }
                    bestLat[i] = lat;
                    const f32 profile =
                        0.55f + 0.45f * (1.0f - lat * lat);
                    riverFlow[i] = dir * river.flowSpeed * profile;
                    riverLevel[i] =
                        glm::mix(a.surface, b.surface, t);
                }
            }
        }
        // Fold this river's claim into the cross-river blend.
        for (const u32 i : touched) {
            const f32 weight = 1.0f - bestLat[i];
            sumFlow[i] += riverFlow[i] * weight;
            sumWeight[i] += weight;
            map.surface[i] = glm::max(map.surface[i], riverLevel[i]);
            bestLat[i] = 2.0f; // reset for the next river
        }
    }

    // Depth + final flow, wet texels only — the single height() pass.
    for (size_t i = 0; i < cells; ++i) {
        if (map.surface[i] <= kWaterInfoDry) {
            continue;
        }
        const i32 col = static_cast<i32>(i % size);
        const i32 row = static_cast<i32>(i / size);
        map.depth[i] = glm::max(
            map.surface[i] - height(worldX(col), worldZ(row)), 0.0f);
        if (!sumWeight.empty() && sumWeight[i] > 0.0f) {
            map.flow[i] = sumFlow[i] / sumWeight[i];
        }
    }
    return map;
}

} // namespace render::terrain
