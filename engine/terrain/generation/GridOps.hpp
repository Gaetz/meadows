#pragma once

#include <cmath>

#include "engine/core/Defines.hpp"
#include "engine/terrain/TerrainBase.hpp" // catmullRom
#include "engine/terrain/generation/TerrainGen.hpp"

// Shared grid toolkit of the generation passes: the 8-connected
// neighbourhood, border-clamped bilinear sampling, and the two-pass 3x3
// chamfer relaxation. Pure functions over GridSpec grids — bit-identical
// replacements for the per-file copies they absorbed.

namespace render::terraingen {

// 4 orthogonal then 4 diagonal neighbours; distances in texel units
// (callers scale by texelSize where meters are needed).
struct Neighbour {
    i32 dx;
    i32 dz;
    f32 dist;
};
inline constexpr Neighbour kNeighbours8[8] = {
    { -1, 0, 1.0f },         { 1, 0, 1.0f },
    { 0, -1, 1.0f },         { 0, 1, 1.0f },
    { -1, -1, 1.41421356f }, { 1, -1, 1.41421356f },
    { -1, 1, 1.41421356f },  { 1, 1, 1.41421356f },
};

// Bilinear sample of an n x n grid at GRID coords (texels), clamped to
// the border.
inline f32 bilinearGrid(const GridSpec& spec, const vector<f32>& grid,
                        f32 u, f32 v) {
    const f32 cu = glm::clamp(u, 0.0f, static_cast<f32>(spec.n - 1));
    const f32 cv = glm::clamp(v, 0.0f, static_cast<f32>(spec.n - 1));
    const u32 u0 = glm::min(static_cast<u32>(cu), spec.n - 2);
    const u32 v0 = glm::min(static_cast<u32>(cv), spec.n - 2);
    const f32 tu = cu - static_cast<f32>(u0);
    const f32 tv = cv - static_cast<f32>(v0);
    const auto at = [&](u32 cx, u32 cz) {
        return grid[static_cast<size_t>(cz) * spec.n + cx];
    };
    const f32 a = at(u0, v0) + (at(u0 + 1, v0) - at(u0, v0)) * tu;
    const f32 b =
        at(u0, v0 + 1) + (at(u0 + 1, v0 + 1) - at(u0, v0 + 1)) * tu;
    return a + (b - a) * tv;
}

// The same sample addressed in WORLD meters.
inline f32 bilinearWorld(const GridSpec& spec, const vector<f32>& grid,
                         f32 wx, f32 wz) {
    return bilinearGrid(spec, grid, (wx - spec.originX) / spec.texelSize,
                        (wz - spec.originZ) / spec.texelSize);
}

// Bicubic (Catmull-Rom) sample of an n x n grid at GRID coords,
// border-clamped taps.
inline f32 bicubicGrid(const GridSpec& spec, const vector<f32>& grid,
                       f32 u, f32 v) {
    const f32 fu = std::floor(u);
    const f32 fv = std::floor(v);
    const i32 iu = static_cast<i32>(fu);
    const i32 iv = static_cast<i32>(fv);
    const f32 tu = u - fu;
    const f32 tv = v - fv;
    const auto at = [&](i32 cx, i32 cz) {
        cx = glm::clamp(cx, 0, static_cast<i32>(spec.n) - 1);
        cz = glm::clamp(cz, 0, static_cast<i32>(spec.n) - 1);
        return grid[static_cast<size_t>(cz) * spec.n +
                    static_cast<size_t>(cx)];
    };
    f32 rows[4];
    for (i32 j = 0; j < 4; ++j) {
        const i32 cz = iv - 1 + j;
        rows[j] = terrain::catmullRom(at(iu - 1, cz), at(iu, cz),
                                      at(iu + 1, cz), at(iu + 2, cz), tu);
    }
    return terrain::catmullRom(rows[0], rows[1], rows[2], rows[3], tv);
}

// Deterministic 3x3 box blur (row-major, border-shrunk window).
inline vector<f32> boxBlur3(const GridSpec& spec, const vector<f32>& src) {
    const i32 n = static_cast<i32>(spec.n);
    vector<f32> dst(spec.cells());
    for (i32 z = 0; z < n; ++z) {
        for (i32 x = 0; x < n; ++x) {
            f32 sum = 0.0f;
            i32 count = 0;
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    const i32 cx = x + dx;
                    const i32 cz = z + dz;
                    if (cx < 0 || cx >= n || cz < 0 || cz >= n) {
                        continue;
                    }
                    sum += src[static_cast<size_t>(cz) * n + cx];
                    ++count;
                }
            }
            dst[static_cast<size_t>(z) * n + x] =
                sum / static_cast<f32>(count);
        }
    }
    return dst;
}

// Two-pass 3x3 chamfer relaxation over a SEEDED distance field (0 at the
// sources, huge elsewhere), width x height cells, distances in texel
// units. Callers seed, sweep, then post-process (scale, sign-combine).
inline void chamferSweep(vector<f32>& d, i32 width, i32 height) {
    const auto at = [&](i32 cx, i32 cz) -> f32& {
        return d[static_cast<size_t>(cz) * static_cast<size_t>(width) +
                 cx];
    };
    const auto relax = [&](i32 cx, i32 cz, i32 ox, i32 oz, f32 w) {
        const i32 px = cx + ox;
        const i32 pz = cz + oz;
        if (px < 0 || pz < 0 || px >= width || pz >= height) {
            return;
        }
        at(cx, cz) = glm::min(at(cx, cz), at(px, pz) + w);
    };
    constexpr f32 kOrtho = 1.0f;
    constexpr f32 kDiag = 1.41421356f;
    for (i32 cz = 0; cz < height; ++cz) {
        for (i32 cx = 0; cx < width; ++cx) {
            relax(cx, cz, -1, 0, kOrtho);
            relax(cx, cz, 0, -1, kOrtho);
            relax(cx, cz, -1, -1, kDiag);
            relax(cx, cz, 1, -1, kDiag);
        }
    }
    for (i32 cz = height - 1; cz >= 0; --cz) {
        for (i32 cx = width - 1; cx >= 0; --cx) {
            relax(cx, cz, 1, 0, kOrtho);
            relax(cx, cz, 0, 1, kOrtho);
            relax(cx, cz, 1, 1, kDiag);
            relax(cx, cz, -1, 1, kDiag);
        }
    }
}

} // namespace render::terraingen
