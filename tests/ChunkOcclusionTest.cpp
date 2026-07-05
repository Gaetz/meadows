#include <doctest/doctest.h>

#include "engine/render/landscape/ChunkOcclusion.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/render/landscape/TerrainSystem.hpp"

using render::ChunkOcclusion;

namespace {

u64 keyOf(i32 cx, i32 cz) {
    return (static_cast<u64>(static_cast<u32>(cx)) << 32) |
           static_cast<u32>(cz);
}

// Real terrain, real camera: build the input from the noise itself, chunk
// tops sampled the same way the mesher would (coarse grid max).
f32 chunkTop(const render::TerrainParams& params, i32 cx, i32 cz) {
    f32 top = -1e9f;
    for (u32 gz = 0; gz <= 8; ++gz) {
        for (u32 gx = 0; gx <= 8; ++gx) {
            const f32 x = (static_cast<f32>(cx) + gx / 8.0f) *
                          render::TerrainSystem::kChunkSize;
            const f32 z = (static_cast<f32>(cz) + gz / 8.0f) *
                          render::TerrainSystem::kChunkSize;
            top = glm::max(top, render::terrain::height(params, x, z));
        }
    }
    return top;
}

} // namespace

TEST_CASE("occlusion is deterministic and conservative") {
    render::TerrainParams params;
    ChunkOcclusion::Input input;
    input.params = params;
    // Camera low in the world, so distant chunks can dip under the horizon.
    input.cameraPos = { 32.0f, render::terrain::height(params, 32.0f, 32.0f) + 2.0f,
                        32.0f };
    for (i32 cz = -12; cz <= 12; ++cz) {
        for (i32 cx = -12; cx <= 12; ++cx) {
            input.chunkTops.emplace(keyOf(cx, cz), chunkTop(params, cx, cz));
        }
    }

    const ChunkOcclusion::Result a = render::computeOcclusion(input);
    const ChunkOcclusion::Result b = render::computeOcclusion(input);
    CHECK(a.occluded == b.occluded); // pure function

    // Conservative basics: the camera's own chunk and its neighbors are
    // never culled, and no occluded chunk is adjacent to the camera.
    for (i32 dz = -2; dz <= 2; ++dz) {
        for (i32 dx = -2; dx <= 2; ++dx) {
            CHECK_FALSE(a.occluded.contains(keyOf(dx, dz)));
        }
    }
    // With a ground-level camera in hilly terrain, SOMETHING far should be
    // occluded (the whole point of the system)...
    CHECK(a.occluded.size() > 0);
    // ...but never everything: the skyline always survives. (The exact
    // ratio is a property of the terrain, not of the code — at ground
    // level in mountains most of the ring genuinely hides.)
    CHECK(a.occluded.size() < input.chunkTops.size());
}

TEST_CASE("extreme targets: towers never occluded, pits always occluded") {
    // Occluders come from the terrain noise; the TARGET height comes from
    // the chunkTops table — so hand-authored extremes pin the target math.
    render::TerrainParams params;
    ChunkOcclusion::Input input;
    input.params = params;
    const f32 ground = render::terrain::height(params, 0.0f, 0.0f);
    input.cameraPos = { 0.0f, ground + 2.0f, 0.0f };

    // A 2000 m tower far away: over any natural horizon, never culled.
    input.chunkTops.emplace(keyOf(12, 0), 2000.0f);
    // A chunk whose drawable top is 1000 m BELOW anything: the flattest
    // horizon still tops it (even +14 m of prop headroom doesn't help).
    input.chunkTops.emplace(keyOf(12, 3), -1000.0f);

    const ChunkOcclusion::Result result = render::computeOcclusion(input);
    CHECK_FALSE(result.occluded.contains(keyOf(12, 0)));
    CHECK(result.occluded.contains(keyOf(12, 3)));
}
