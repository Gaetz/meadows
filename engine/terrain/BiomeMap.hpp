#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

// Biomes: a small table of terrain-character params indexed by a u8 id.
// The id over a point comes from the baked region's biome mask channel
// (sandbox tiles) or from a painted index map (scenario mode); id 0 is
// always the NEUTRAL biome — everything then behaves exactly like the
// pre-biome rules, which is what keeps existing scatter masks unmoved.
// Headless home (HeightPatches rationale): gameplay reads climate through
// the same data the splat/scatter side uses.

namespace render {

struct BiomeParams {
    // Terrain character (materialWeightsAt + scatter).
    f32 snowLineOffset { 0.0f }; // meters added to kSnowLine
    f32 rockiness { 0.0f };      // 0..1: rock claims gentler slopes
    f32 sandiness { 0.0f };      // 0..1: wider sand band above the shore
    f32 grassPresence { 1.0f };  // multiplier on the grass weight
    f32 detailAmplitudeScale { 1.0f };
    // Climate (gameplay/survival reads these through biomeAt).
    f32 temperature { 0.0f }; // -1 freezing .. +1 scorching
    f32 wetness { 0.0f };     // 0 arid .. 1 swamp
    u32 vegetationSet { 0 };  // index into scene species tables
};

struct BiomeSet {
    vector<BiomeParams> table; // [0] = neutral; ids clamp into this

    // Optional painted index map (scenario mode; sandbox tiles carry
    // their ids in the region's biome mask instead).
    f32 originX { 0.0f };
    f32 originZ { 0.0f };
    f32 texelSize { 16.0f };
    u32 width { 0 };
    u32 height { 0 };
    vector<u8> indices; // nearest-sampled

    u8 paintedIndexAt(f32 x, f32 z) const {
        if (indices.empty() || width == 0 || height == 0) {
            return 0;
        }
        const i32 cx = glm::clamp(
            static_cast<i32>((x - originX) / texelSize + 0.5f), 0,
            static_cast<i32>(width) - 1);
        const i32 cz = glm::clamp(
            static_cast<i32>((z - originZ) / texelSize + 0.5f), 0,
            static_cast<i32>(height) - 1);
        return indices[static_cast<size_t>(cz) * width +
                       static_cast<size_t>(cx)];
    }
};

} // namespace render
