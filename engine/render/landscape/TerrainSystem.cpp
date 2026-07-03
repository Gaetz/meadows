#include "engine/render/landscape/TerrainSystem.hpp"

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

void TerrainSystem::create(rhi::Device& device, ShaderLibrary& shaders) {
    const vector<u32> indices = buildChunkIndices();
    indexCount = static_cast<u32>(indices.size());
    indexBuffer = device.createBuffer({ .usage = rhi::BufferUsage::Index,
                                        .size = indices.size() * sizeof(u32) },
                                      indices.data());

    for (i32 cz = -kGridHalfExtent; cz <= kGridHalfExtent; ++cz) {
        for (i32 cx = -kGridHalfExtent; cx <= kGridHalfExtent; ++cx) {
            const vector<MeshVertex> vertices =
                buildChunkVertices(params, cx, cz);
            chunks.push_back(
                { .cx = cx,
                  .cz = cz,
                  .vertexBuffer = device.createBuffer(
                      { .usage = rhi::BufferUsage::Vertex,
                        .size = vertices.size() * sizeof(MeshVertex) },
                      vertices.data()) });
        }
    }

    shaders.load(kTerrainShader, { { "FrameUbo", 0 } });
    buildPipeline(device, shaders);
}

void TerrainSystem::destroy(rhi::Device& device) {
    device.destroyPipeline(pipeline);
    for (const Chunk& chunk : chunks) {
        device.destroyBuffer(chunk.vertexBuffer);
    }
    chunks.clear();
    device.destroyBuffer(indexBuffer);
    pipeline = {};
    indexBuffer = {};
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
    for (const Chunk& chunk : chunks) {
        cmd.setVertexBuffer(0, chunk.vertexBuffer);
        cmd.drawIndexed(indexCount);
    }
}

} // namespace render
