#include "engine/render/landscape/VegetationSystem.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/Jobs.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/render/landscape/TerrainSystem.hpp"
#include "engine/render/landscape/TreeGenerator.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {

constexpr const char* kTreeShader = "tree";
constexpr f32 kTreeSpacing = 5.5f; // meters between scatter candidates

u32 hashU32(u32 v) {
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}

struct HashRng {
    u32 state;
    f32 next() { // [0, 1)
        state = hashU32(state);
        return static_cast<f32>(state) * (1.0f / 4294967296.0f);
    }
};

// Forest belts: broad noise thresholded, so woods come as forests and
// clearings — not confetti.
f32 forestMask(u32 seed, f32 x, f32 z) {
    const f32 broad =
        terrain::noise01(seed ^ 0x3c6ef372u, x / 105.0f, z / 105.0f);
    const f32 detail =
        terrain::noise01(seed ^ 0xa54ff53au, x / 28.0f, z / 28.0f);
    return glm::smoothstep(0.50f, 0.60f, broad * 0.78f + detail * 0.22f);
}

} // namespace

VegetationSystem::VariantBuckets scatterTrees(const TerrainParams& params,
                                              i32 cx, i32 cz) {
    const f32 originX = static_cast<f32>(cx) * TerrainSystem::kChunkSize;
    const f32 originZ = static_cast<f32>(cz) * TerrainSystem::kChunkSize;
    const u32 perSide =
        static_cast<u32>(TerrainSystem::kChunkSize / kTreeSpacing);

    VegetationSystem::VariantBuckets buckets;
    for (u32 gz = 0; gz < perSide; ++gz) {
        for (u32 gx = 0; gx < perSide; ++gx) {
            HashRng rng { hashU32(params.seed ^ 0x2545f491u) ^
                          hashU32(static_cast<u32>(cx * 83492791 ^
                                                   cz * 297121507) ^
                                  (gz * perSide + gx)) };
            const f32 x = originX + (static_cast<f32>(gx) + rng.next()) *
                                        kTreeSpacing;
            const f32 z = originZ + (static_cast<f32>(gz) + rng.next()) *
                                        kTreeSpacing;
            const f32 forest = forestMask(params.seed, x, z);
            if (forest < 0.05f || rng.next() >= forest * 0.85f) {
                continue;
            }
            const f32 h = terrain::height(params, x, z);
            const Vec3 n = terrain::normal(params, x, z);
            // Trees want gentle, grassy, mid-altitude ground: above the
            // beach, below the alpine line, off the cliffs.
            const f32 slope = 1.0f - n.y;
            if (h < params.seaLevel + 3.0f || h > 92.0f || slope > 0.22f) {
                continue;
            }
            const u32 variant = static_cast<u32>(
                rng.next() * static_cast<f32>(VegetationSystem::kTreeVariants));
            buckets[glm::min(variant,
                             VegetationSystem::kTreeVariants - 1)]
                .push_back({
                    // Sink slightly so leaning trunks never float on slopes.
                    .positionScale = { x, h - 0.15f, z,
                                       0.8f + rng.next() * 0.6f },
                    .params = { rng.next() * 6.2831853f, // yaw
                                rng.next(),              // tint jitter
                                rng.next() * 6.2831853f, // sway phase
                                0.0f },
                });
        }
    }
    return buckets;
}

void VegetationSystem::create(rhi::Device& device, ShaderLibrary& shaders,
                              core::JobSystem& jobSystem, u32 terrainSeed) {
    jobs = &jobSystem;
    shared = std::make_shared<Shared>();
    createVariantMeshes(device, terrainSeed);
    shaders.load(kTreeShader, { { "FrameUbo", 0 } });
    buildPipeline(device, shaders);
}

void VegetationSystem::createVariantMeshes(rhi::Device& device,
                                           u32 terrainSeed) {
    for (u32 i = 0; i < kTreeVariants; ++i) {
        const MeshData mesh = generateTree(hashU32(terrainSeed) + i * 977u);
        variantMeshes[i].indexCount = static_cast<u32>(mesh.indices.size());
        variantMeshes[i].vertexBuffer = device.createBuffer(
            { .usage = rhi::BufferUsage::Vertex,
              .size = mesh.vertices.size() * sizeof(MeshVertex) },
            mesh.vertices.data());
        variantMeshes[i].indexBuffer = device.createBuffer(
            { .usage = rhi::BufferUsage::Index,
              .size = mesh.indices.size() * sizeof(u32) },
            mesh.indices.data());
    }
}

void VegetationSystem::destroyVariantMeshes(rhi::Device& device) {
    for (VariantMesh& variant : variantMeshes) {
        device.destroyBuffer(variant.indexBuffer);
        device.destroyBuffer(variant.vertexBuffer);
        variant = {};
    }
}

void VegetationSystem::destroy(rhi::Device& device) {
    ++generation;
    for (auto& [key, chunk] : chunks) {
        device.destroyBuffer(chunk.instanceBuffer);
    }
    chunks.clear();
    instances = 0;
    device.destroyPipeline(pipeline);
    pipeline = {};
    destroyVariantMeshes(device);
}

void VegetationSystem::regenerate(rhi::Device& device, u32 terrainSeed) {
    ++generation;
    for (auto& [key, chunk] : chunks) {
        device.destroyBuffer(chunk.instanceBuffer);
    }
    chunks.clear();
    instances = 0;
    destroyVariantMeshes(device);
    createVariantMeshes(device, terrainSeed);
}

void VegetationSystem::update(rhi::Device& device, const TerrainParams& params,
                              const Vec3& cameraPos) {
    // Budgeted uploads.
    u32 uploads = 0;
    BuiltChunk built;
    while (uploads < kMaxUploadsPerFrame && shared->built.tryPop(built)) {
        if (built.generation != generation) {
            continue;
        }
        const auto it = chunks.find(keyOf(built.cx, built.cz));
        if (it == chunks.end() || it->second.resident) {
            continue;
        }
        Chunk& chunk = it->second;
        vector<Instance> packed;
        for (u32 v = 0; v < kTreeVariants; ++v) {
            chunk.firstInstance[v] = static_cast<u32>(packed.size());
            chunk.counts[v] = static_cast<u32>(built.buckets[v].size());
            packed.insert(packed.end(), built.buckets[v].begin(),
                          built.buckets[v].end());
        }
        chunk.total = static_cast<u32>(packed.size());
        if (chunk.total > 0) {
            chunk.instanceBuffer = device.createBuffer(
                { .usage = rhi::BufferUsage::Vertex,
                  .size = packed.size() * sizeof(Instance) },
                packed.data());
        }
        chunk.resident = true;
        instances += chunk.total;
        ++uploads;
    }

    // Request missing chunks.
    const i32 camCx = static_cast<i32>(
        std::floor(cameraPos.x / TerrainSystem::kChunkSize));
    const i32 camCz = static_cast<i32>(
        std::floor(cameraPos.z / TerrainSystem::kChunkSize));
    for (i32 dz = -kViewRadius; dz <= kViewRadius; ++dz) {
        for (i32 dx = -kViewRadius; dx <= kViewRadius; ++dx) {
            const i32 cx = camCx + dx;
            const i32 cz = camCz + dz;
            if (chunks.contains(keyOf(cx, cz))) {
                continue;
            }
            chunks.emplace(keyOf(cx, cz), Chunk {});
            jobs->enqueue([sharedRef = shared, params, cx, cz,
                           gen = generation] {
                sharedRef->built.push(
                    { cx, cz, gen, scatterTrees(params, cx, cz) });
            });
        }
    }

    // Evict beyond hysteresis.
    for (auto it = chunks.begin(); it != chunks.end();) {
        const i32 cx = static_cast<i32>(it->first >> 32);
        const i32 cz = static_cast<i32>(it->first & 0xffffffffu);
        if (std::max(std::abs(cx - camCx), std::abs(cz - camCz)) <=
            kEvictRadius) {
            ++it;
            continue;
        }
        if (it->second.resident) {
            device.destroyBuffer(it->second.instanceBuffer);
            instances -= it->second.total;
        }
        it = chunks.erase(it);
    }
}

void VegetationSystem::buildPipeline(rhi::Device& device,
                                     ShaderLibrary& shaders) {
    if (pipeline.id != 0) {
        device.destroyPipeline(pipeline);
    }
    pipeline = device.createPipeline(
        { .shader = shaders.get(kTreeShader),
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
                                    .offset = offsetof(MeshVertex, color) } } },
                { .stride = sizeof(Instance),
                  .stepMode = rhi::VertexStepMode::Instance,
                  .attributes = { { .location = 4,
                                    .format = rhi::VertexFormat::F32x4,
                                    .offset = offsetof(Instance,
                                                       positionScale) },
                                  { .location = 5,
                                    .format = rhi::VertexFormat::F32x4,
                                    .offset = offsetof(Instance, params) } } } },
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Less },
          .cull = rhi::CullMode::Back });
    shaderGeneration = shaders.generation(kTreeShader);
}

void VegetationSystem::refreshPipeline(rhi::Device& device,
                                       ShaderLibrary& shaders) {
    if (shaders.generation(kTreeShader) != shaderGeneration) {
        buildPipeline(device, shaders);
    }
}

void VegetationSystem::draw(rhi::CommandBuffer& cmd,
                            rhi::BindGroupHandle frameBindGroup) {
    cmd.setPipeline(pipeline);
    cmd.setBindGroup(0, frameBindGroup);
    // Variant-major: bind each tree mesh once, then one instanced draw per
    // chunk holding that variant (firstInstance = offset into the chunk's
    // variant-sorted instance buffer; requires baseInstance, present on 4.6).
    for (u32 v = 0; v < kTreeVariants; ++v) {
        bool meshBound = false;
        for (const auto& [key, chunk] : chunks) {
            if (!chunk.resident || chunk.counts[v] == 0) {
                continue;
            }
            if (!meshBound) {
                cmd.setVertexBuffer(0, variantMeshes[v].vertexBuffer);
                cmd.setIndexBuffer(variantMeshes[v].indexBuffer,
                                   rhi::IndexFormat::U32);
                meshBound = true;
            }
            cmd.setVertexBuffer(1, chunk.instanceBuffer);
            cmd.drawIndexed(variantMeshes[v].indexCount, chunk.counts[v], 0,
                            chunk.firstInstance[v]);
        }
    }
}

} // namespace render
