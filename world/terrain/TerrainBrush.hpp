#pragma once

#include <functional>
#include <memory>
#include <unordered_map>

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"
#include "engine/terrain/HeightPatches.hpp"

// The terrain brush, headless: edits WORKING delta grids (one per 64 m
// chunk, 65x65 samples at 1 m, edge samples shared with the neighbour)
// and publishes a fresh immutable HeightPatches. Shared by the editor's
// sculpt tool and the earth spirit (docs/SPELLS.md, chantier ESPRITS
// E2): a dam raised in front of a spring pools it — the scene's
// republish tells the water sim its ground changed.

namespace world {

enum class BrushKind : u8 { Raise, Lower, Flatten, Smooth };

struct BrushParams {
    BrushKind kind { BrushKind::Raise };
    f32 radius { 6.0f };   // metres
    f32 strength { 2.0f }; // metres per second (raise/lower)
    f32 flattenTarget { 0.0f }; // Flatten: the height to level toward
};

using BrushGrids = std::unordered_map<u64, render::HeightPatch>;
using LiveHeightFn = std::function<f32(f32 x, f32 z)>; // base + published

// The working grid of chunk (cx, cz): seeded from the published overlay
// when that chunk is already authored, zero otherwise.
render::HeightPatch& brushGridFor(BrushGrids& grids,
                                  const render::HeightPatches* published,
                                  i32 cx, i32 cz, u32 samples = 65);

// One brush application at `center` for `dt` seconds. Flatten reads the
// live height through `liveHeight` (null = flatten is a no-op).
void applyTerrainBrush(BrushGrids& grids,
                       const render::HeightPatches* published, f32 chunkSize,
                       const BrushParams& brush, const Vec2& center, f32 dt,
                       const LiveHeightFn& liveHeight = nullptr);

// A wall: raises every sample within `halfWidth` of the segment ab by
// `height` x a smooth falloff of the perpendicular distance (round end
// caps). One application, no accumulation along the segment.
void applyTerrainWall(BrushGrids& grids, const render::HeightPatches* published,
                      f32 chunkSize, const Vec2& a, const Vec2& b,
                      f32 halfWidth, f32 height);

// The next immutable overlay: the published chunks overridden by the
// working grids; `changed` receives the keys the caller must re-mesh.
std::shared_ptr<render::HeightPatches> publishBrushGrids(
    const BrushGrids& grids, const render::HeightPatches* published,
    f32 chunkSize, vector<u64>& changed);

// The working delta at a sample position (integer metres); 0 where no
// grid holds it. Tests and the earth spell's height probe.
f32 brushDeltaAt(const BrushGrids& grids, f32 chunkSize, i32 x, i32 z);

} // namespace world
