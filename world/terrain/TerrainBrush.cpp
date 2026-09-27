#include "world/terrain/TerrainBrush.hpp"

#include <cmath>

#include <glm/glm.hpp>

namespace world {

render::HeightPatch& brushGridFor(BrushGrids& grids,
                                  const render::HeightPatches* published,
                                  i32 cx, i32 cz, u32 samples) {
    const u64 key = render::HeightPatches::keyOf(cx, cz);
    const auto it = grids.find(key);
    if (it != grids.end()) {
        return it->second;
    }
    if (published) {
        if (const auto existing = published->chunks.find(key);
            existing != published->chunks.end()) {
            return grids.emplace(key, existing->second).first->second;
        }
    }
    render::HeightPatch fresh;
    fresh.samples = samples;
    fresh.deltas.assign(static_cast<size_t>(samples) * samples, 0.0f);
    return grids.emplace(key, std::move(fresh)).first->second;
}

void applyTerrainBrush(BrushGrids& grids,
                       const render::HeightPatches* published, f32 chunkSize,
                       const BrushParams& brush, const Vec2& center, f32 dt,
                       const LiveHeightFn& liveHeight) {
    if (brush.radius <= 0.0f || chunkSize <= 0.0f) {
        return;
    }
    const i32 minCx = static_cast<i32>(std::floor((center.x - brush.radius) / chunkSize));
    const i32 maxCx = static_cast<i32>(std::floor((center.x + brush.radius) / chunkSize));
    const i32 minCz = static_cast<i32>(std::floor((center.y - brush.radius) / chunkSize));
    const i32 maxCz = static_cast<i32>(std::floor((center.y + brush.radius) / chunkSize));
    for (i32 cz = minCz; cz <= maxCz; ++cz) {
        for (i32 cx = minCx; cx <= maxCx; ++cx) {
            render::HeightPatch& grid = brushGridFor(grids, published, cx, cz);
            const f32 step = chunkSize / static_cast<f32>(grid.samples - 1);
            for (u32 row = 0; row < grid.samples; ++row) {
                for (u32 col = 0; col < grid.samples; ++col) {
                    const f32 x = static_cast<f32>(cx) * chunkSize +
                                  static_cast<f32>(col) * step;
                    const f32 z = static_cast<f32>(cz) * chunkSize +
                                  static_cast<f32>(row) * step;
                    const f32 dx = x - center.x;
                    const f32 dz = z - center.y;
                    const f32 dist = std::sqrt(dx * dx + dz * dz);
                    if (dist >= brush.radius) {
                        continue;
                    }
                    const f32 t = 1.0f - dist / brush.radius;
                    const f32 falloff = t * t * (3.0f - 2.0f * t);
                    f32& delta = grid.deltas[row * grid.samples + col];
                    switch (brush.kind) {
                    case BrushKind::Raise:
                        delta += brush.strength * falloff * dt;
                        break;
                    case BrushKind::Lower:
                        delta -= brush.strength * falloff * dt;
                        break;
                    case BrushKind::Flatten: {
                        // Toward the target against the LIVE height (base
                        // + published patch); the working delta absorbs
                        // the gap.
                        if (!liveHeight) {
                            break;
                        }
                        const f32 gap = brush.flattenTarget - liveHeight(x, z);
                        delta += gap * glm::min(2.5f * falloff * dt, 1.0f);
                        break;
                    }
                    case BrushKind::Smooth: {
                        const u32 c0 = col > 0 ? col - 1 : col;
                        const u32 c1 = glm::min(col + 1, grid.samples - 1);
                        const u32 r0 = row > 0 ? row - 1 : row;
                        const u32 r1 = glm::min(row + 1, grid.samples - 1);
                        const f32 average =
                            (grid.deltas[row * grid.samples + c0] +
                             grid.deltas[row * grid.samples + c1] +
                             grid.deltas[r0 * grid.samples + col] +
                             grid.deltas[r1 * grid.samples + col]) *
                            0.25f;
                        delta += (average - delta) *
                                 glm::min(4.0f * falloff * dt, 1.0f);
                        break;
                    }
                    }
                }
            }
        }
    }
}

std::shared_ptr<render::HeightPatches> publishBrushGrids(
    const BrushGrids& grids, const render::HeightPatches* published,
    f32 chunkSize, vector<u64>& changed) {
    auto next = std::make_shared<render::HeightPatches>();
    next->chunkSize = chunkSize;
    if (published) {
        next->chunks = published->chunks;
    }
    changed.reserve(changed.size() + grids.size());
    for (const auto& [key, grid] : grids) {
        next->chunks[key] = grid;
        changed.push_back(key);
    }
    return next;
}

f32 brushDeltaAt(const BrushGrids& grids, f32 chunkSize, i32 x, i32 z) {
    const i32 cx = static_cast<i32>(std::floor(static_cast<f32>(x) / chunkSize));
    const i32 cz = static_cast<i32>(std::floor(static_cast<f32>(z) / chunkSize));
    const auto it = grids.find(render::HeightPatches::keyOf(cx, cz));
    if (it == grids.end()) {
        return 0.0f;
    }
    const render::HeightPatch& grid = it->second;
    const f32 step = chunkSize / static_cast<f32>(grid.samples - 1);
    const i32 col = static_cast<i32>(std::lround(
        (static_cast<f32>(x) - static_cast<f32>(cx) * chunkSize) / step));
    const i32 row = static_cast<i32>(std::lround(
        (static_cast<f32>(z) - static_cast<f32>(cz) * chunkSize) / step));
    if (col < 0 || row < 0 || col >= static_cast<i32>(grid.samples) ||
        row >= static_cast<i32>(grid.samples)) {
        return 0.0f;
    }
    return grid.deltas[static_cast<size_t>(row) * grid.samples + col];
}

} // namespace world
