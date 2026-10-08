#include "engine/terrain/generation/MapGrid.hpp"

#include <cmath>
#include <unordered_map>

namespace render::terraingen {

namespace {

constexpr u32 kSaltBorderStyle = 0xb02de125u;

} // namespace

MapEdgeStyle mapBorderStyle(u32 seed, i32 lineIndex, i32 cellCross,
                            bool vertical) {
    u64 h = 14695981039346656037ull;
    const auto mix = [&h](u64 v) {
        for (int byte = 0; byte < 8; ++byte) {
            h ^= (v >> (byte * 8)) & 0xFF;
            h *= 1099511628211ull;
        }
    };
    mix(seed ^ kSaltBorderStyle);
    mix(static_cast<u64>(static_cast<u32>(lineIndex)));
    mix(static_cast<u64>(static_cast<u32>(cellCross)));
    mix(vertical ? 0x76ull : 0x68ull);
    return (h & 1ull) != 0ull ? MapEdgeStyle::Ridges
                              : MapEdgeStyle::Sea;
}

MapEdgeStyle mapBorderSegmentStyle(const WorldLayerParams& world,
                                   f32 mapSize, i32 lineIndex,
                                   i32 cellCross, bool vertical) {
    const MapEdgeStyle proposed =
        mapBorderStyle(world.seed, lineIndex, cellCross, vertical);
    if (proposed != MapEdgeStyle::Sea) {
        return proposed;
    }
    // Sea veto: a sea arm only stands where the world already reads
    // coastal along the segment — deep inland it demotes to the
    // canonical land-land border (Ridges). Memoized per segment; the
    // key reads every world param so a pupitre edit never serves a
    // stale veto.
    thread_local std::unordered_map<u64, MapEdgeStyle> memo;
    u64 key = static_cast<u64>(static_cast<u32>(lineIndex)) |
              (static_cast<u64>(static_cast<u32>(cellCross)) << 32);
    key ^= vertical ? 0x9e3779b97f4a7c15ull : 0xc2b2ae3d27d4eb4full;
    key ^= static_cast<u64>(static_cast<i64>(mapSize)) << 3;
    key ^= hashParams(world) * 0x9e3779b97f4a7c15ull;
    if (const auto it = memo.find(key); it != memo.end()) {
        return it->second;
    }
    constexpr u32 kSamples = 9;
    u32 oceanish = 0;
    for (u32 i = 0; i < kSamples; ++i) {
        const f32 along =
            (static_cast<f32>(cellCross) +
             (static_cast<f32>(i) + 0.5f) / static_cast<f32>(kSamples)) *
            mapSize;
        const f32 lineAt = static_cast<f32>(lineIndex) * mapSize;
        const f32 sx = vertical ? lineAt : along;
        const f32 sz = vertical ? along : lineAt;
        const WorldSample w = worldSampleAt(world, sx, sz);
        if (w.sea || w.base < 2.0f) {
            ++oceanish;
        }
    }
    const MapEdgeStyle resolved =
        static_cast<f32>(oceanish) <
                kMapBorderSeaVetoOceanFrac * static_cast<f32>(kSamples)
            ? MapEdgeStyle::Ridges
            : MapEdgeStyle::Sea;
    memo.emplace(key, resolved);
    return resolved;
}

} // namespace render::terraingen
