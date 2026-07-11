#include "engine/render/landscape/GrassSystem.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/Hash.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/render/landscape/TerrainSystem.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {

constexpr const char* kGrassShader = "grass";

// BotW-style patch layout: grass gathers in dense clumps with bare meadow
// between them. Two noise scales — broad patches and small clump detail —
// thresholded into a [0,1] density mask (0 = bare, 1 = heart of a patch).
// Every knob lives in GrassScatterTuning (render panel, "Grass").
f32 patchMask(u32 seed, const GrassScatterTuning& tuning, f32 x, f32 z) {
    const f32 broad = terrain::noise01(seed ^ 0x1f123bb5u,
                                       x / tuning.patchBroadScale,
                                       z / tuning.patchBroadScale);
    const f32 detail = terrain::noise01(seed ^ 0x9d2c5680u,
                                        x / tuning.patchDetailScale,
                                        z / tuning.patchDetailScale);
    return glm::smoothstep(tuning.patchThresholdLo, tuning.patchThresholdHi,
                           broad * 0.72f + detail * 0.28f);
}

// hashU32 / HashRng now live in engine/core/Hash.hpp (shared scatter hash family).
using core::hashU32;
using core::HashRng;

// One blade (redo #2, the SimonDev Quick_Grass model): 6 straight
// segments, sides at ±1 with NO baked taper — grass.vert shapes the
// 1-t^2 taper and the forward arc per vertex. 13 vertices, 11 triangles;
// one-sided strip, cull off (the fragment normals are ground-dominated,
// no back-face flip needed).
constexpr f32 kBladeVertices[] = {
    // side,  t
    -1.0f, 0.0f,       1.0f, 0.0f,
    -1.0f, 1.0f/6.0f,  1.0f, 1.0f/6.0f,
    -1.0f, 2.0f/6.0f,  1.0f, 2.0f/6.0f,
    -1.0f, 3.0f/6.0f,  1.0f, 3.0f/6.0f,
    -1.0f, 4.0f/6.0f,  1.0f, 4.0f/6.0f,
    -1.0f, 5.0f/6.0f,  1.0f, 5.0f/6.0f,
     0.0f, 1.0f, // the tip
};
constexpr u16 kBladeIndices[] = {
    0, 1, 2,   1, 3, 2,   2, 3, 4,   3, 5, 4,
    4, 5, 6,   5, 7, 6,   6, 7, 8,   7, 9, 8,
    8, 9, 10,  9, 11, 10, 10, 11, 12,
};

} // namespace

vector<GrassSystem::Instance> scatterGrass(const TerrainParams& params,
                                           const GrassScatterTuning& tuning,
                                           i32 cx, i32 cz) {
    const f32 originX = static_cast<f32>(cx) * TerrainSystem::kChunkSize;
    const f32 originZ = static_cast<f32>(cz) * TerrainSystem::kChunkSize;
    const f32 spacing = glm::max(0.05f, tuning.spacing);
    const u32 perSide =
        static_cast<u32>(TerrainSystem::kChunkSize / spacing);

    // CELL-MAJOR scatter (startup-cost fix: the 0.15 m grid made the
    // per-candidate noise evals saturate every worker for seconds at
    // boot). The masks vary over METERS, not centimeters — patch,
    // material and normal are evaluated once per cell, and the terrain
    // height on the cell-corner lattice (bilinear per blade — at or
    // below the render mesh's own sampling error). Bare cells cost two
    // noise evals total; only the cheap per-blade jitter runs per
    // candidate.
    constexpr u32 kCell = 4; // candidates per cell side (0.6 m cells)
    const u32 cells = (perSide + kCell - 1) / kCell;
    const f32 cellSize = spacing * static_cast<f32>(kCell);

    vector<f32> cornerH((cells + 1) * (cells + 1));
    for (u32 gz = 0; gz <= cells; ++gz) {
        for (u32 gx = 0; gx <= cells; ++gx) {
            cornerH[gz * (cells + 1) + gx] = terrain::height(
                params, originX + static_cast<f32>(gx) * cellSize,
                originZ + static_cast<f32>(gz) * cellSize);
        }
    }

    vector<GrassSystem::Instance> result;
    result.reserve(perSide * perSide / 4);
    for (u32 cgz = 0; cgz < cells; ++cgz) {
        for (u32 cgx = 0; cgx < cells; ++cgx) {
            // Near-BINARY presence (7.8quinquies): even moderately inside
            // the mask the clump is at FULL density (the solid volume);
            // only the rim thins, fast, so patches keep their silhouette.
            const f32 patch = patchMask(
                params.seed, tuning,
                originX + (static_cast<f32>(cgx) + 0.5f) * cellSize,
                originZ + (static_cast<f32>(cgz) + 0.5f) * cellSize);
            const f32 presence = glm::smoothstep(tuning.presenceLo,
                                                 tuning.presenceHi, patch);
            if (presence < 0.02f) {
                continue;
            }
            const f32 h00 = cornerH[cgz * (cells + 1) + cgx];
            const f32 h10 = cornerH[cgz * (cells + 1) + cgx + 1];
            const f32 h01 = cornerH[(cgz + 1) * (cells + 1) + cgx];
            const f32 h11 = cornerH[(cgz + 1) * (cells + 1) + cgx + 1];
            // Cell normal from its corners — grass shading barely
            // perturbs it, lattice precision is plenty.
            const Vec3 n = glm::normalize(
                Vec3 { -(h10 - h00 + h11 - h01) / (2.0f * cellSize), 1.0f,
                       -(h01 - h00 + h11 - h10) / (2.0f * cellSize) });
            const f32 hMid = 0.25f * (h00 + h10 + h01 + h11);
            // HARD material cutoff: grass only on solidly grassy ground —
            // never on the sand/snow/rock transition fringes.
            if (terrain::materialWeights(params, hMid, n).grass <
                tuning.materialCutoff) {
                continue;
            }
            for (u32 sz = 0; sz < kCell; ++sz) {
                for (u32 sx = 0; sx < kCell; ++sx) {
                    const u32 gx = cgx * kCell + sx;
                    const u32 gz = cgz * kCell + sz;
                    if (gx >= perSide || gz >= perSide) {
                        continue;
                    }
                    HashRng rng { hashU32(params.seed ^ 0x6b79a3f1u) ^
                                  hashU32(static_cast<u32>(cx * 73856093 ^
                                                           cz * 19349663) ^
                                          (gz * perSide + gx)) };
                    const f32 x = originX +
                                  (static_cast<f32>(gx) + rng.next()) *
                                      spacing;
                    const f32 z = originZ +
                                  (static_cast<f32>(gz) + rng.next()) *
                                      spacing;
                    if (rng.next() >= presence) {
                        continue;
                    }
                    const f32 fx =
                        (x - (originX + static_cast<f32>(cgx) * cellSize)) /
                        cellSize;
                    const f32 fz =
                        (z - (originZ + static_cast<f32>(cgz) * cellSize)) /
                        cellSize;
                    const f32 h = glm::mix(glm::mix(h00, h10, fx),
                                           glm::mix(h01, h11, fx), fz);
                    // Height: near-uniform inside the volume so the top
                    // reads as one surface; the rim droops shorter, and
                    // ~12% of blades overshoot — the tips poking above
                    // the mass (the BotW tell).
                    f32 scale = (0.70f + rng.next() * 0.30f) *
                                (0.55f + 0.45f * presence);
                    if (rng.next() < 0.12f) {
                        scale *= 1.35f;
                    }
                    // Rim blades lean/curve harder — the clump spills
                    // over its sides instead of ending in a wall.
                    const f32 lean = glm::min(
                        1.0f, rng.next() + (1.0f - presence) * 0.6f);
                    result.push_back({
                        .positionScale = { x, h, z, scale },
                        .params = { rng.next() * 6.2831853f, // yaw
                                    rng.next() * 6.2831853f, // flutter
                                    rng.next(),              // tint jitter
                                    lean },                  // lean amount
                        .groundNormal = { n, 0.0f }, // 7.8bis: BotW shading
                    });
                }
            }
        }
    }
    // Sort by the density-LOD keep key (flutter phase — uniform random
    // per blade): grass.vert clips blades whose key exceeds the metric
    // density curve, so a PREFIX of this buffer is EXACTLY the set the
    // shader keeps. draw() cuts the prefix with the same curve — the
    // vertex shader never even sees the tail it would have clipped.
    std::sort(result.begin(), result.end(),
              [](const GrassSystem::Instance& a,
                 const GrassSystem::Instance& b) {
                  return a.params.y < b.params.y;
              });
    return result;
}

void GrassSystem::create(rhi::Device& device, ShaderLibrary& shaders,
                         core::JobSystem& jobSystem) {
    streamer.create(jobSystem);

    bladeVertexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex, .size = sizeof(kBladeVertices) },
        kBladeVertices) };
    bladeIndexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Index, .size = sizeof(kBladeIndices) },
        kBladeIndices) };
    bladeIndexCount = static_cast<u32>(std::size(kBladeIndices));

    shaders.load(kGrassShader, { { "FrameUbo", 0 } },
                 { { "uShadowMap", 1 } });
    buildPipeline(device, shaders);
}

void GrassSystem::destroy(rhi::Device& device) {
    (void)device; // U3-7: Unique handles free through their device
    streamer.invalidateAll([](Chunk&) {});
    instances = 0;
    pipeline.reset();
    bladeIndexBuffer.reset();
    bladeVertexBuffer.reset();
}

void GrassSystem::regenerate(rhi::Device& device) {
    (void)device; // U3-7: the erases free the instance buffers
    streamer.invalidateAll([](Chunk&) {});
    instances = 0;
}

void GrassSystem::invalidateChunks(rhi::Device& device,
                                   const vector<u64>& keys) {
    for (const u64 key : keys) {
        const auto it = streamer.chunks.find(key);
        if (it == streamer.chunks.end() || !it->second.resident) {
            continue; // missing, or still streaming in (no stale swap)
        }
        instances -= it->second.instanceCount;
        // update() re-requests + re-scatters with new heights (the erase
        // frees the instance buffer, U3-7).
        streamer.chunks.erase(it);
    }
}

void GrassSystem::update(rhi::Device& device, const TerrainParams& params,
                         const Vec3& cameraPos) {
    // Budgeted uploads (U3-1: the ring mechanics live in ChunkStreamer;
    // this lambda is the grass-specific accept + GPU upload).
    streamer.pump(kMaxUploadsPerFrame, 0.0, [&](u64 key, auto& built) {
        const auto it = streamer.chunks.find(key);
        if (it == streamer.chunks.end() || it->second.resident) {
            return false;
        }
        Chunk& chunk = it->second;
        chunk.instanceCount = static_cast<u32>(built.payload.size());
        if (chunk.instanceCount > 0) {
            chunk.instanceBuffer = { device, device.createBuffer(
                { .usage = rhi::BufferUsage::Vertex,
                  .size = built.payload.size() * sizeof(Instance) },
                built.payload.data()) };
            chunk.minY = built.payload[0].positionScale.y;
            chunk.maxY = chunk.minY;
            for (const Instance& instance : built.payload) {
                chunk.minY = glm::min(chunk.minY, instance.positionScale.y);
                chunk.maxY = glm::max(chunk.maxY, instance.positionScale.y);
            }
        }
        chunk.resident = true;
        instances += chunk.instanceCount;
        return true;
    });

    // Request missing chunks in the grass ring — nearest first, budgeted
    // (the rest is re-detected next frame; the state IS the queue).
    const i32 camCx = chunkCoordOf(cameraPos.x, TerrainSystem::kChunkSize);
    const i32 camCz = chunkCoordOf(cameraPos.z, TerrainSystem::kChunkSize);
    streamer.requestMissing(
        camCx, camCz, kViewRadius, kMaxRequestsPerFrame,
        [&](i32 cx, i32 cz, i32, i32) {
            return !streamer.chunks.contains(chunkKey(cx, cz));
        },
        [&](i32 cx, i32 cz, i32, i32) {
            streamer.chunks.emplace(chunkKey(cx, cz), Chunk {});
            streamer.enqueueBuild(cx, cz,
                                  [params, tuning = scatterTuning, cx, cz] {
                return scatterGrass(params, tuning, cx, cz);
            });
        });

    // Evict beyond hysteresis.
    streamer.evictFar(camCx, camCz, kEvictRadius, [&](Chunk& chunk) {
        // U3-7: the erase frees the instance buffer.
        if (chunk.resident) {
            instances -= chunk.instanceCount;
        }
    });
}

void GrassSystem::buildPipeline(rhi::Device& device, ShaderLibrary& shaders) {
    pipeline = { device, device.createPipeline( // U3-7: frees the old one
        { .shader = shaders.get(kGrassShader),
          .vertexBuffers =
              { { .stride = 2 * sizeof(f32),
                  .attributes = { { .location = 0,
                                    .format = rhi::VertexFormat::F32x2,
                                    .offset = 0 } } },
                { .stride = sizeof(Instance),
                  .stepMode = rhi::VertexStepMode::Instance,
                  .attributes = { { .location = 2,
                                    .format = rhi::VertexFormat::F32x4,
                                    .offset = offsetof(Instance,
                                                       positionScale) },
                                  { .location = 3,
                                    .format = rhi::VertexFormat::F32x4,
                                    .offset = offsetof(Instance, params) },
                                  { .location = 4,
                                    .format = rhi::VertexFormat::F32x4,
                                    .offset = offsetof(
                                        Instance, groundNormal) } } } },
          // Pure geometry, opaque: depth-write on so blades sort against the
          // terrain and each other; visible from both sides.
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Less },
          .cull = rhi::CullMode::None }) };
    shaderGeneration = shaders.generation(kGrassShader);
}

void GrassSystem::refreshPipeline(rhi::Device& device,
                                  ShaderLibrary& shaders) {
    if (shaders.generation(kGrassShader) != shaderGeneration) {
        buildPipeline(device, shaders);
    }
}

void GrassSystem::draw(rhi::CommandBuffer& cmd,
                       rhi::BindGroupHandle frameBindGroup,
                       rhi::BindGroupHandle shadowBindGroup,
                       const Vec3& cameraPos, const Frustum* frustum) {
    cmd.setPipeline(pipeline);
    cmd.setBindGroup(0, frameBindGroup);
    if (shadowBindGroup.id != 0) {
        cmd.setBindGroup(2, shadowBindGroup);
    }
    cmd.setVertexBuffer(0, bladeVertexBuffer);
    cmd.setIndexBuffer(bladeIndexBuffer, rhi::IndexFormat::U16);
    struct Draw {
        f32 nearest;
        const Chunk* chunk;
        u32 count;
    };
    vector<Draw> draws;
    draws.reserve(streamer.chunks.size());
    for (const auto& [key, chunk] : streamer.chunks) {
        if (!chunk.resident || chunk.instanceCount == 0) {
            continue;
        }
        const f32 minX =
            static_cast<f32>(chunkKeyCx(key)) * TerrainSystem::kChunkSize;
        const f32 minZ =
            static_cast<f32>(chunkKeyCz(key)) * TerrainSystem::kChunkSize;
        const f32 dx = glm::max(
            glm::max(minX - cameraPos.x,
                     cameraPos.x - (minX + TerrainSystem::kChunkSize)),
            0.0f);
        const f32 dz = glm::max(
            glm::max(minZ - cameraPos.z,
                     cameraPos.z - (minZ + TerrainSystem::kChunkSize)),
            0.0f);
        const f32 nearest = std::sqrt(dx * dx + dz * dz);
        if (nearest > renderTuning.fadeEnd) {
            continue; // every blade fully sunk into the ground
        }
        if (frustum) {
            // Headroom over the blade roots (height × scale + wind).
            if (!frustum->intersectsAabb(
                    { minX, chunk.minY - 0.5f, minZ },
                    { minX + TerrainSystem::kChunkSize,
                      chunk.maxY + renderTuning.bladeHeight + 0.8f,
                      minZ + TerrainSystem::kChunkSize })) {
                continue;
            }
        }
        // Density LOD prefix: instances are SORTED by the keep key, so
        // drawing the first N is exactly the set grass.vert keeps at
        // this chunk's NEAREST distance (density is monotonic in dist —
        // every blade in the chunk is at least that far). Same curve as
        // the shader (both read the SAME tuning — grass.vert through
        // uGrassLodInfo), +3% margin for the key sampling noise.
        const f32 thin = glm::smoothstep(renderTuning.thinStart,
                                         renderTuning.thinEnd, nearest);
        const f32 density = glm::min(
            1.0f, glm::mix(1.0f, renderTuning.farDensity, thin) + 0.03f);
        const u32 count = glm::min(
            chunk.instanceCount,
            1u + static_cast<u32>(
                     static_cast<f32>(chunk.instanceCount) * density));
        draws.push_back({ nearest, &chunk, count });
    }
    // Front-to-back: near blades fill the depth buffer first, so the
    // grass behind them early-z-rejects instead of shading over.
    std::sort(draws.begin(), draws.end(),
              [](const Draw& a, const Draw& b) {
                  return a.nearest < b.nearest;
              });
    for (const Draw& d : draws) {
        cmd.setVertexBuffer(1, d.chunk->instanceBuffer);
        cmd.drawIndexed(bladeIndexCount, d.count);
    }
}

} // namespace render
