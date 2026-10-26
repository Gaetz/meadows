#include "engine/terrain/generation/MapGrid.hpp"
#include "engine/core/Hash.hpp"

#include <cmath>
#include <unordered_map>

namespace render::terraingen {

namespace {

constexpr u32 kSaltBorderStyle = 0xb02de125u;

} // namespace

MapEdgeStyle mapBorderStyle(u32 seed, i32 lineIndex, i32 cellCross,
                            bool vertical) {
    u64 h = core::kFnv64Basis;
    const auto mix = [&h](u64 v) {
        for (int byte = 0; byte < 8; ++byte) {
            h ^= (v >> (byte * 8)) & 0xFF;
            h *= core::kFnv64Prime;
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
    if (!world.borderFromWorld && proposed != MapEdgeStyle::Sea) {
        return proposed;
    }
    // The world decides (passe EAU E2): ocean majority = Sea, massif
    // mean = Ridges, else Open — the landscape continues. The legacy
    // path keeps only the Sea veto of its hashed proposal. Memoized
    // per segment, for ONE set of world params (an exact copy, compared
    // on every call) so a pupitre edit never serves a stale style.
    struct SegmentMemo {
        WorldLayerParams world;
        bool valid { false };
        std::unordered_map<u64, MapEdgeStyle> styles;
    };
    thread_local SegmentMemo memo;
    if (!memo.valid || !(memo.world == world)) {
        memo.styles.clear();
        memo.world = world;
        memo.valid = true;
    }
    u64 key = static_cast<u64>(static_cast<u32>(lineIndex)) |
              (static_cast<u64>(static_cast<u32>(cellCross)) << 32);
    key ^= vertical ? 0x9e3779b97f4a7c15ull : 0xc2b2ae3d27d4eb4full;
    key ^= static_cast<u64>(static_cast<i64>(mapSize)) << 3;
    if (const auto it = memo.styles.find(key); it != memo.styles.end()) {
        return it->second;
    }
    constexpr u32 kSamples = 9;
    u32 oceanish = 0;
    f32 massifSum = 0.0f;
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
        massifSum += w.massif;
    }
    const bool oceanLine =
        static_cast<f32>(oceanish) >=
        kMapBorderSeaVetoOceanFrac * static_cast<f32>(kSamples);
    MapEdgeStyle resolved;
    if (!world.borderFromWorld) {
        // Legacy: a hashed Sea proposal stands only on a coastal line.
        resolved = oceanLine ? MapEdgeStyle::Sea : MapEdgeStyle::Ridges;
    } else if (oceanLine) {
        resolved = MapEdgeStyle::Sea;
    } else if (massifSum >=
               world.borderMassifMin * static_cast<f32>(kSamples)) {
        resolved = MapEdgeStyle::Ridges;
    } else {
        resolved = MapEdgeStyle::Open;
    }
    memo.styles.emplace(key, resolved);
    return resolved;
}

} // namespace render::terraingen
