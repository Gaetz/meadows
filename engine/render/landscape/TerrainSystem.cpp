#include "engine/render/landscape/TerrainSystem.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {

constexpr const char* kTerrainShader = "terrain";

// Per-vertex material color until the splatting brick replaces it with
// blended tiling textures: sand at the shoreline, grass on plains, rock on
// slopes, snow on high flats.
Vec3 terrainColor(f32 height, const Vec3& normal, f32 seaLevel) {
    constexpr Vec3 kSand { 0.76f, 0.70f, 0.50f };
    constexpr Vec3 kGrass { 0.33f, 0.51f, 0.21f };
    constexpr Vec3 kRock { 0.46f, 0.44f, 0.42f };
    constexpr Vec3 kSnow { 0.93f, 0.95f, 0.97f };

    const f32 slope = 1.0f - normal.y;
    Vec3 color = glm::mix(
        kSand, kGrass,
        glm::smoothstep(seaLevel + 1.0f, seaLevel + 4.0f, height));
    color = glm::mix(color, kRock, glm::smoothstep(0.18f, 0.35f, slope));
    const f32 snowiness = glm::smoothstep(110.0f, 140.0f, height) *
                          (1.0f - glm::smoothstep(0.25f, 0.45f, slope));
    return glm::mix(color, kSnow, snowiness);
}

i32 chunkCoordOf(f32 worldCoord) {
    return static_cast<i32>(
        std::floor(worldCoord / TerrainSystem::kChunkSize));
}

} // namespace

vector<MeshVertex> buildChunkVertices(const TerrainParams& params, i32 cx,
                                      i32 cz) {
    constexpr u32 kVertsPerSide = TerrainSystem::kChunkQuads + 1;
    constexpr f32 kStep = TerrainSystem::kChunkSize / TerrainSystem::kChunkQuads;
    const f32 originX = static_cast<f32>(cx) * TerrainSystem::kChunkSize;
    const f32 originZ = static_cast<f32>(cz) * TerrainSystem::kChunkSize;

    vector<MeshVertex> vertices;
    vertices.reserve(kVertsPerSide * kVertsPerSide);
    for (u32 gz = 0; gz < kVertsPerSide; ++gz) {
        for (u32 gx = 0; gx < kVertsPerSide; ++gx) {
            const f32 x = originX + static_cast<f32>(gx) * kStep;
            const f32 z = originZ + static_cast<f32>(gz) * kStep;
            const f32 y = terrain::height(params, x, z);
            const Vec3 n = terrain::normal(params, x, z);
            vertices.push_back({
                .position = { x, y, z },
                .normal = n,
                .uv = { x / TerrainSystem::kChunkSize,
                        z / TerrainSystem::kChunkSize },
                .color = terrainColor(y, n, params.seaLevel),
            });
        }
    }
    return vertices;
}

vector<u32> buildChunkIndices() {
    constexpr u32 kQuads = TerrainSystem::kChunkQuads;
    constexpr u32 kVertsPerSide = kQuads + 1;
    vector<u32> indices;
    indices.reserve(kQuads * kQuads * 6);
    for (u32 gz = 0; gz < kQuads; ++gz) {
        for (u32 gx = 0; gx < kQuads; ++gx) {
            const u32 i00 = gz * kVertsPerSide + gx;
            const u32 i10 = i00 + 1;
            const u32 i01 = i00 + kVertsPerSide;
            const u32 i11 = i01 + 1;
            // CCW seen from above (+Y), so CullMode::Back keeps the top.
            indices.insert(indices.end(), { i00, i01, i11, i00, i11, i10 });
        }
    }
    return indices;
}

void TerrainSystem::create(rhi::Device& device, ShaderLibrary& shaders,
                           core::JobSystem& jobSystem) {
    jobs = &jobSystem;
    shared = std::make_shared<Shared>();

    const vector<u32> indices = buildChunkIndices();
    indexCount = static_cast<u32>(indices.size());
    indexBuffer = device.createBuffer({ .usage = rhi::BufferUsage::Index,
                                        .size = indices.size() * sizeof(u32) },
                                      indices.data());

    shaders.load(kTerrainShader, { { "FrameUbo", 0 } });
    buildPipeline(device, shaders);
}

void TerrainSystem::destroy(rhi::Device& device) {
    // Orphaned worker jobs keep pushing into `shared` harmlessly; results die
    // with the last reference (TextureCache teardown pattern).
    ++generation;
    for (auto& [key, chunk] : chunks) {
        device.destroyBuffer(chunk.vertexBuffer);
    }
    chunks.clear();
    resident = 0;
    pending = 0;
    device.destroyPipeline(pipeline);
    device.destroyBuffer(indexBuffer);
    pipeline = {};
    indexBuffer = {};
}

void TerrainSystem::regenerate(rhi::Device& device) {
    ++generation;
    for (auto& [key, chunk] : chunks) {
        device.destroyBuffer(chunk.vertexBuffer);
    }
    chunks.clear();
    resident = 0;
    pending = 0;
    // update() re-requests the ring with the new params next frame.
}

void TerrainSystem::update(rhi::Device& device, const Vec3& cameraPos) {
    pumpUploads(device);
    requestMissing(cameraPos);
    evictFar(device, cameraPos);
}

void TerrainSystem::pumpUploads(rhi::Device& device) {
    lastUploads = 0;
    BuiltChunk built;
    while (lastUploads < kMaxUploadsPerFrame && shared->built.tryPop(built)) {
        if (built.generation != generation) {
            continue; // stale: regenerated or torn down since the request
        }
        const auto it = chunks.find(keyOf(built.cx, built.cz));
        if (it == chunks.end() || it->second.state == ChunkState::Resident) {
            continue; // evicted while in flight, or a duplicate result
        }
        it->second.vertexBuffer = device.createBuffer(
            { .usage = rhi::BufferUsage::Vertex,
              .size = built.vertices.size() * sizeof(MeshVertex) },
            built.vertices.data());
        it->second.state = ChunkState::Resident;
        ++resident;
        --pending;
        ++lastUploads;
    }
}

void TerrainSystem::requestMissing(const Vec3& cameraPos) {
    const i32 camCx = chunkCoordOf(cameraPos.x);
    const i32 camCz = chunkCoordOf(cameraPos.z);

    struct Candidate {
        i32 cx, cz, dist2;
    };
    vector<Candidate> missing;
    for (i32 dz = -kViewRadius; dz <= kViewRadius; ++dz) {
        for (i32 dx = -kViewRadius; dx <= kViewRadius; ++dx) {
            const i32 cx = camCx + dx;
            const i32 cz = camCz + dz;
            if (!chunks.contains(keyOf(cx, cz))) {
                missing.push_back({ cx, cz, dx * dx + dz * dz });
            }
        }
    }
    // Center-out: the terrain under the camera arrives first, holes stay at
    // the horizon.
    std::sort(missing.begin(), missing.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.dist2 < b.dist2;
              });

    for (const Candidate& c : missing) {
        chunks.emplace(keyOf(c.cx, c.cz), Chunk {});
        ++pending;
        jobs->enqueue([sharedRef = shared, chunkParams = params, cx = c.cx,
                       cz = c.cz, gen = generation] {
            sharedRef->built.push({ cx, cz, gen,
                                    buildChunkVertices(chunkParams, cx, cz) });
        });
    }
}

void TerrainSystem::evictFar(rhi::Device& device, const Vec3& cameraPos) {
    const i32 camCx = chunkCoordOf(cameraPos.x);
    const i32 camCz = chunkCoordOf(cameraPos.z);
    for (auto it = chunks.begin(); it != chunks.end();) {
        const i32 cx = static_cast<i32>(it->first >> 32);
        const i32 cz = static_cast<i32>(it->first & 0xffffffffu);
        const i32 dist = std::max(std::abs(cx - camCx), std::abs(cz - camCz));
        if (dist <= kEvictRadius) {
            ++it;
            continue;
        }
        if (it->second.state == ChunkState::Resident) {
            device.destroyBuffer(it->second.vertexBuffer);
            --resident;
        } else {
            --pending; // in-flight result will be dropped on arrival
        }
        it = chunks.erase(it);
    }
}

void TerrainSystem::buildPipeline(rhi::Device& device, ShaderLibrary& shaders) {
    if (pipeline.id != 0) {
        device.destroyPipeline(pipeline);
    }
    pipeline = device.createPipeline(
        { .shader = shaders.get(kTerrainShader),
          .vertexBuffers =
              { { .stride = sizeof(MeshVertex),
                  .attributes = { { .location = 0,
                                    .format = rhi::VertexFormat::F32x3,
                                    .offset = offsetof(MeshVertex, position) },
                                  { .location = 1,
                                    .format = rhi::VertexFormat::F32x3,
                                    .offset = offsetof(MeshVertex, normal) },
                                  { .location = 2,
                                    .format = rhi::VertexFormat::F32x2,
                                    .offset = offsetof(MeshVertex, uv) },
                                  { .location = 3,
                                    .format = rhi::VertexFormat::F32x3,
                                    .offset = offsetof(MeshVertex, color) } } } },
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Less },
          .cull = rhi::CullMode::Back });
    shaderGeneration = shaders.generation(kTerrainShader);
}

void TerrainSystem::refreshPipeline(rhi::Device& device,
                                    ShaderLibrary& shaders) {
    if (shaders.generation(kTerrainShader) != shaderGeneration) {
        buildPipeline(device, shaders);
    }
}

void TerrainSystem::draw(rhi::CommandBuffer& cmd,
                         rhi::BindGroupHandle frameBindGroup) {
    cmd.setPipeline(pipeline);
    cmd.setIndexBuffer(indexBuffer, rhi::IndexFormat::U32);
    cmd.setBindGroup(0, frameBindGroup);
    for (const auto& [key, chunk] : chunks) {
        if (chunk.state != ChunkState::Resident) {
            continue;
        }
        cmd.setVertexBuffer(0, chunk.vertexBuffer);
        cmd.drawIndexed(indexCount);
    }
}

} // namespace render
