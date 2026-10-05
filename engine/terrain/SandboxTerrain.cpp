#include "engine/terrain/SandboxTerrain.hpp"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

namespace render {

namespace {

// Bilinear read of a 64 m overview; false outside its coverage.
bool overviewHeight(const terraingen::GridSpec& g, const vector<f32>& heights,
                    f32 x, f32 z, f32& out) {
    if (heights.empty() || g.n < 2) {
        return false;
    }
    const f32 u = (x - g.originX) / g.texelSize;
    const f32 v = (z - g.originZ) / g.texelSize;
    if (u < 0.0f || v < 0.0f || u > static_cast<f32>(g.n - 1) ||
        v > static_cast<f32>(g.n - 1)) {
        return false;
    }
    const u32 c0 = glm::min(static_cast<u32>(u), g.n - 2);
    const u32 r0 = glm::min(static_cast<u32>(v), g.n - 2);
    const f32 tu = u - static_cast<f32>(c0);
    const f32 tv = v - static_cast<f32>(r0);
    const auto at = [&](u32 c, u32 r) {
        return heights[static_cast<size_t>(r) * g.n + c];
    };
    const f32 a = glm::mix(at(c0, r0), at(c0 + 1, r0), tu);
    const f32 b = glm::mix(at(c0, r0 + 1), at(c0 + 1, r0 + 1), tu);
    out = glm::mix(a, b, tv);
    return true;
}

} // namespace

f32 sandboxFallbackHeight(const SandboxTerrain& sb, f32 x, f32 z) {
    // Inside a baked map's coverage the fallback is the map's own 64 m
    // overview (bilinear) — the analytic mirror cannot follow a
    // globally carved valley network. The active map first, then the
    // cached neighbours (the horizon past the rim).
    f32 h = 0.0f;
    if (overviewHeight(sb.overviewGrid, sb.overview, x, z, h)) {
        return h;
    }
    for (const SandboxTerrain::Overview& o : sb.neighbourOverviews) {
        if (overviewHeight(o.grid, o.heights, x, z, h)) {
            return h;
        }
    }
    const terraingen::ProceduralControls controls { sb.controls };
    const f32 ha = terraingen::macroHeightAnalytic(controls, sb.macro, x, z);
    // Bounded-map border transitions: the same pure border-line shaping
    // the bakes use (identity when grid.valid is false).
    return terraingen::applyMapGridShape(controls, sb.macro, sb.grid, x, z,
                                         ha);
}

bool spawnCandidateOk(f32 h, f32 seaLevel, u8 biome) {
    return h > seaLevel + 8.0f && h < 95.0f && biome == 0;
}

std::optional<Vec3> probeMapSpawn(
    const SandboxTerrain& sb, i32 mapX, i32 mapZ, f32 seaLevel,
    const std::function<bool(f32 x, f32 z)>& wet) {
    const terraingen::ProceduralControls controls { sb.controls };
    const auto wetAt = [&](f32 x, f32 z) { return wet && wet(x, z); };
    const f32 size = sb.grid.mapSize;
    const f32 mapMid = (static_cast<f32>(mapX) + 0.5f) * size;
    const f32 mapMidZ = (static_cast<f32>(mapZ) + 0.5f) * size;
    const f32 mapReach = size * 0.5f - terraingen::kMapBorderMountainHalf;
    // From the centre outward in 350 m rings, stopping short of the rim
    // band (an 8 km map leaves ~3 km of reach).
    for (f32 radius = 600.0f; radius <= mapReach - 200.0f;
         radius += 350.0f) {
        for (u32 step = 0; step < 16; ++step) {
            const f32 angle =
                radius * 0.0137f + static_cast<f32>(step) * 0.3927f;
            const f32 x = mapMid + std::cos(angle) * radius;
            const f32 z = mapMidZ + std::sin(angle) * radius;
            const f32 h = sandboxFallbackHeight(sb, x, z);
            if (spawnCandidateOk(h, seaLevel, controls.at(x, z).biome) &&
                !wetAt(x, z)) {
                return Vec3 { x, h, z };
            }
        }
    }
    // No meadow on this map: the gentlest dry spot over the same rings
    // (a plateau, a valley floor), never a slope nor a lake shore.
    // Flatness = the worst height step over 100 m in 8 directions on
    // the fallback ground, every sample dry; the nearer ring wins a
    // near-tie so the start stays central.
    std::optional<Vec3> best;
    f32 bestScore = 1.0e30f;
    for (f32 radius = 0.0f; radius <= mapReach - 200.0f; radius += 350.0f) {
        const u32 steps = radius > 0.0f ? 16u : 1u;
        for (u32 step = 0; step < steps; ++step) {
            const f32 angle =
                radius * 0.0137f + static_cast<f32>(step) * 0.3927f;
            const f32 x = mapMid + std::cos(angle) * radius;
            const f32 z = mapMidZ + std::sin(angle) * radius;
            const f32 h = sandboxFallbackHeight(sb, x, z);
            if (h <= seaLevel + 8.0f || wetAt(x, z)) {
                continue;
            }
            f32 worstStep = 0.0f;
            bool shore = false;
            for (u32 k = 0; k < 8 && !shore; ++k) {
                const f32 a = static_cast<f32>(k) * 0.7853982f;
                const f32 xk = x + std::cos(a) * 100.0f;
                const f32 zk = z + std::sin(a) * 100.0f;
                shore = wetAt(xk, zk);
                worstStep = std::max(
                    worstStep,
                    std::abs(sandboxFallbackHeight(sb, xk, zk) - h));
            }
            if (shore) {
                continue;
            }
            const f32 score = worstStep + radius * 0.002f;
            if (score < bestScore) {
                bestScore = score;
                best = Vec3 { x, h, z };
            }
        }
    }
    return best;
}

} // namespace render
