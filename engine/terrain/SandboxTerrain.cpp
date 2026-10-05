#include "engine/terrain/SandboxTerrain.hpp"

#include <cmath>

#include <glm/glm.hpp>

namespace render {

f32 sandboxFallbackHeight(const SandboxTerrain& sb, f32 x, f32 z) {
    // Inside a baked map's coverage the fallback is the map's own 64 m
    // overview (bilinear) — the analytic mirror cannot follow a
    // globally carved valley network.
    if (!sb.overview.empty() && sb.overviewGrid.n >= 2) {
        const terraingen::GridSpec& g = sb.overviewGrid;
        const f32 u = (x - g.originX) / g.texelSize;
        const f32 v = (z - g.originZ) / g.texelSize;
        if (u >= 0.0f && v >= 0.0f && u <= static_cast<f32>(g.n - 1) &&
            v <= static_cast<f32>(g.n - 1)) {
            const u32 c0 = glm::min(static_cast<u32>(u), g.n - 2);
            const u32 r0 = glm::min(static_cast<u32>(v), g.n - 2);
            const f32 tu = u - static_cast<f32>(c0);
            const f32 tv = v - static_cast<f32>(r0);
            const auto at = [&](u32 c, u32 r) {
                return sb.overview[static_cast<size_t>(r) * g.n + c];
            };
            const f32 a = glm::mix(at(c0, r0), at(c0 + 1, r0), tu);
            const f32 b = glm::mix(at(c0, r0 + 1), at(c0 + 1, r0 + 1), tu);
            return glm::mix(a, b, tv);
        }
    }
    const terraingen::ProceduralControls controls { sb.controls };
    const f32 h = terraingen::macroHeightAnalytic(controls, sb.macro, x, z);
    // Bounded-map border transitions: the same pure border-line shaping
    // the bakes use (identity when grid.valid is false).
    return terraingen::applyMapGridShape(controls, sb.macro, sb.grid, x, z,
                                         h);
}

bool spawnCandidateOk(f32 h, f32 seaLevel, u8 biome) {
    return h > seaLevel + 8.0f && h < 95.0f && biome == 0;
}

std::optional<Vec3> probeMapSpawn(const SandboxTerrain& sb, i32 mapX,
                                  i32 mapZ, f32 seaLevel) {
    const terraingen::ProceduralControls controls { sb.controls };
    const f32 size = sb.grid.mapSize;
    const f32 mapMid = (static_cast<f32>(mapX) + 0.5f) * size;
    const f32 mapMidZ = (static_cast<f32>(mapZ) + 0.5f) * size;
    const f32 mapReach = size * 0.5f - terraingen::kMapBorderMountainHalf;
    for (f32 radius = 2600.0f; radius <= glm::min(24000.0f, mapReach);
         radius += 700.0f) {
        for (u32 step = 0; step < 16; ++step) {
            const f32 angle =
                radius * 0.0137f + static_cast<f32>(step) * 0.3927f;
            const f32 x = mapMid + std::cos(angle) * radius;
            const f32 z = mapMidZ + std::sin(angle) * radius;
            const f32 h = sandboxFallbackHeight(sb, x, z);
            if (spawnCandidateOk(h, seaLevel, controls.at(x, z).biome)) {
                return Vec3 { x, h, z };
            }
        }
    }
    return std::nullopt;
}

} // namespace render
