#include "engine/render/landscape/VegetationSystem.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/Hash.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/render/landscape/TerrainSystem.hpp"
#include "engine/render/landscape/TreeGenerator.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {

constexpr const char* kTreeShader = "tree";
constexpr const char* kPropCasterShader = "shadow_prop";
constexpr f32 kTreeSpacing = 4.0f; // meters between scatter candidates

// hashU32 / HashRng now live in engine/core/Hash.hpp (shared scatter hash family).
using core::hashU32;
using core::HashRng;

// Forest belts: broad noise thresholded, so woods come as forests and
// clearings — not confetti.
f32 forestMask(u32 seed, f32 x, f32 z) {
    const f32 broad =
        terrain::noise01(seed ^ 0x3c6ef372u, x / 105.0f, z / 105.0f);
    const f32 detail =
        terrain::noise01(seed ^ 0xa54ff53au, x / 28.0f, z / 28.0f);
    return glm::smoothstep(0.46f, 0.58f, broad * 0.78f + detail * 0.22f);
}

// Bush clumps: medium-frequency blobs so shrubs come in family groups —
// and, crossed with the forest-edge factor, break its isoline into clusters
// instead of tracing it as a string.
f32 bushClumpMask(u32 seed, f32 x, f32 z) {
    const f32 blob = terrain::noise01(seed ^ 0x452821e6u, x / 13.0f,
                                      z / 13.0f);
    return glm::smoothstep(0.52f, 0.66f, blob);
}

} // namespace

VegetationSystem::VariantBuckets scatterProps(const TerrainParams& params,
                                              i32 cx, i32 cz) {
    const f32 originX = static_cast<f32>(cx) * TerrainSystem::kChunkSize;
    const f32 originZ = static_cast<f32>(cz) * TerrainSystem::kChunkSize;
    VegetationSystem::VariantBuckets buckets;

    const auto place = [&](u32 firstVariant, u32 variantCount, HashRng& rng,
                           f32 x, f32 y, f32 z, f32 scaleMin, f32 scaleMax,
                           f32 fadeEnd) {
        const u32 variant =
            firstVariant +
            glm::min(static_cast<u32>(rng.next() *
                                      static_cast<f32>(variantCount)),
                     variantCount - 1);
        buckets[variant].push_back({
            .positionScale = { x, y, z,
                               scaleMin + rng.next() * (scaleMax - scaleMin) },
            .params = { rng.next() * 6.2831853f, // yaw
                        rng.next(),              // tint jitter
                        rng.next() * 6.2831853f, // sway phase
                        fadeEnd },               // per-category view distance
        });
    };
    const auto candidateRng = [&](u32 salt, u32 index) {
        return HashRng { hashU32(params.seed ^ salt) ^
                         hashU32(static_cast<u32>(cx * 83492791 ^
                                                  cz * 297121507) ^
                                 index) };
    };

    // --- Trees: forest belts on gentle grassy mid-altitude ground ------------
    {
        const u32 perSide =
            static_cast<u32>(TerrainSystem::kChunkSize / kTreeSpacing);
        for (u32 i = 0; i < perSide * perSide; ++i) {
            HashRng rng = candidateRng(0x2545f491u, i);
            const f32 x = originX + (static_cast<f32>(i % perSide) +
                                     rng.next()) *
                                        kTreeSpacing;
            const f32 z = originZ + (static_cast<f32>(i / perSide) +
                                     rng.next()) *
                                        kTreeSpacing;
            const f32 forest = forestMask(params.seed, x, z);
            if (forest < 0.05f || rng.next() >= forest * 0.95f) {
                continue;
            }
            const f32 h = terrain::height(params, x, z);
            const Vec3 n = terrain::normal(params, x, z);
            const f32 slope = 1.0f - n.y;
            if (h < params.seaLevel + 3.0f || h > 92.0f || slope > 0.22f) {
                continue;
            }
            // Sink slightly so leaning trunks never float on slopes.
            place(0, VegetationSystem::kTreeVariants, rng, x, h - 0.15f, z,
                  0.8f, 1.4f, 880.0f);
        }
    }

    // --- Rocks: sparse everywhere, denser on rocky/alpine ground -------------
    {
        constexpr f32 kRockSpacing = 7.5f;
        const u32 perSide =
            static_cast<u32>(TerrainSystem::kChunkSize / kRockSpacing);
        for (u32 i = 0; i < perSide * perSide; ++i) {
            HashRng rng = candidateRng(0x8f14ab5du, i);
            const f32 x = originX + (static_cast<f32>(i % perSide) +
                                     rng.next()) *
                                        kRockSpacing;
            const f32 z = originZ + (static_cast<f32>(i / perSide) +
                                     rng.next()) *
                                        kRockSpacing;
            const f32 h = terrain::height(params, x, z);
            const Vec3 n = terrain::normal(params, x, z);
            const f32 slope = 1.0f - n.y;
            if (h < params.seaLevel + 0.5f || slope > 0.55f) {
                continue; // not underwater, not on cliff faces
            }
            const auto weights = terrain::materialWeights(params, h, n);
            // Boulders belong to rocky and alpine ground first, meadows get
            // the occasional loner.
            const f32 chance =
                0.08f + 0.42f * weights.rock + 0.32f * weights.snow;
            if (rng.next() >= chance) {
                continue;
            }
            place(VegetationSystem::kFirstRock,
                  VegetationSystem::kRockVariants, rng, x, h - 0.10f, z,
                  0.5f, 2.0f, 660.0f); // 25% shorter reach than trees
        }
    }

    // --- Bushes: grassy ground, biased toward forest edges -------------------
    {
        constexpr f32 kBushSpacing = 5.0f;
        const u32 perSide =
            static_cast<u32>(TerrainSystem::kChunkSize / kBushSpacing);
        for (u32 i = 0; i < perSide * perSide; ++i) {
            HashRng rng = candidateRng(0xc1d1f0adu, i);
            const f32 x = originX + (static_cast<f32>(i % perSide) +
                                     rng.next()) *
                                        kBushSpacing;
            const f32 z = originZ + (static_cast<f32>(i / perSide) +
                                     rng.next()) *
                                        kBushSpacing;
            const f32 h = terrain::height(params, x, z);
            const Vec3 n = terrain::normal(params, x, z);
            if (terrain::materialWeights(params, h, n).grass < 0.65f) {
                continue;
            }
            // Clumps gate everything (bushes come in family groups); the
            // forest-edge factor then biases WHICH clumps are lush, without
            // tracing the treeline as a string.
            const f32 clump = bushClumpMask(params.seed, x, z);
            if (clump < 0.05f) {
                continue;
            }
            const f32 forest = forestMask(params.seed, x, z);
            const f32 edge = forest * (1.0f - forest) * 4.0f;
            if (rng.next() >= clump * (0.35f + 0.65f * edge)) {
                continue;
            }
            place(VegetationSystem::kFirstBush,
                  VegetationSystem::kBushVariants, rng, x, h - 0.05f, z,
                  0.7f, 1.3f, 660.0f); // small silhouettes: rock reach
        }
    }
    return buckets;
}

void VegetationSystem::create(rhi::Device& device, ShaderLibrary& shaders,
                              core::JobSystem& jobSystem, u32 terrainSeed) {
    jobs = &jobSystem;
    shared = std::make_shared<Shared>();
    createVariantMeshes(device, terrainSeed);
    shaders.load(kTreeShader, { { "FrameUbo", 0 } },
                 { { "uShadowMap", 1 } });
    buildPipeline(device, shaders);
    shaders.load(kPropCasterShader, { { "FrameUbo", 0 }, { "ShadowUbo", 1 } });
    buildCasterPipeline(device, shaders);
}

void VegetationSystem::createVariantMeshes(rhi::Device& device,
                                           u32 terrainSeed) {
    for (u32 i = 0; i < kVariantCount; ++i) {
        if (const auto it = meshOverrides.find(i);
            it != meshOverrides.end()) {
            uploadVariantMesh(device, i, it->second);
            continue;
        }
        const u32 seed = hashU32(terrainSeed) + i * 977u;
        if (i < kFirstRock) {
            uploadVariantMesh(device, i, generateTree(seed, 2));
            uploadLowDetailMesh(device, i, generateTree(seed, 1));
        } else if (i < kFirstBush) {
            uploadVariantMesh(device, i, generateRock(seed));
        } else {
            uploadVariantMesh(device, i, generateBush(seed));
        }
    }
}

void VegetationSystem::uploadVariantMesh(rhi::Device& device, u32 variant,
                                         const MeshData& mesh) {
    variantMeshes[variant].indexCount =
        static_cast<u32>(mesh.indices.size());
    variantMeshes[variant].vertexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = mesh.vertices.size() * sizeof(MeshVertex) },
        mesh.vertices.data());
    variantMeshes[variant].indexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = mesh.indices.size() * sizeof(u32) },
        mesh.indices.data());
}

void VegetationSystem::uploadLowDetailMesh(rhi::Device& device, u32 variant,
                                           const MeshData& mesh) {
    variantMeshes[variant].lowIndexCount =
        static_cast<u32>(mesh.indices.size());
    variantMeshes[variant].lowVertexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = mesh.vertices.size() * sizeof(MeshVertex) },
        mesh.vertices.data());
    variantMeshes[variant].lowIndexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = mesh.indices.size() * sizeof(u32) },
        mesh.indices.data());
}

void VegetationSystem::overrideVariantMesh(rhi::Device& device, u32 variant,
                                           MeshData mesh) {
    if (variant >= kVariantCount || mesh.vertices.empty() ||
        mesh.indices.empty()) {
        return;
    }
    if (variantMeshes[variant].vertexBuffer.id != 0) {
        device.destroyBuffer(variantMeshes[variant].indexBuffer);
        device.destroyBuffer(variantMeshes[variant].vertexBuffer);
    }
    if (variantMeshes[variant].lowVertexBuffer.id != 0) {
        // Authored meshes come as ONE detail level.
        device.destroyBuffer(variantMeshes[variant].lowIndexBuffer);
        device.destroyBuffer(variantMeshes[variant].lowVertexBuffer);
    }
    variantMeshes[variant] = {};
    uploadVariantMesh(device, variant, mesh);
    meshOverrides[variant] = std::move(mesh);
}

void VegetationSystem::destroyVariantMeshes(rhi::Device& device) {
    for (VariantMesh& variant : variantMeshes) {
        device.destroyBuffer(variant.indexBuffer);
        device.destroyBuffer(variant.vertexBuffer);
        if (variant.lowVertexBuffer.id != 0) {
            device.destroyBuffer(variant.lowIndexBuffer);
            device.destroyBuffer(variant.lowVertexBuffer);
        }
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
    device.destroyPipeline(casterPipeline);
    casterPipeline = {};
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
        for (u32 v = 0; v < kVariantCount; ++v) {
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
            chunk.minY = packed[0].positionScale.y;
            chunk.maxY = chunk.minY;
            for (const Instance& instance : packed) {
                chunk.minY = glm::min(chunk.minY, instance.positionScale.y);
                chunk.maxY = glm::max(chunk.maxY, instance.positionScale.y);
            }
        }
        chunk.resident = true;
        instances += chunk.total;
        ++uploads;
    }

    // Request missing chunks — nearest first, budgeted (the rest is
    // re-detected next frame; the state IS the queue). See TerrainSystem:
    // the unbudgeted ring edge was part of the fast-travel stutter.
    const i32 camCx = static_cast<i32>(
        std::floor(cameraPos.x / TerrainSystem::kChunkSize));
    const i32 camCz = static_cast<i32>(
        std::floor(cameraPos.z / TerrainSystem::kChunkSize));
    struct Candidate {
        i32 cx, cz, dist2;
    };
    vector<Candidate> wanted;
    for (i32 dz = -kViewRadius; dz <= kViewRadius; ++dz) {
        for (i32 dx = -kViewRadius; dx <= kViewRadius; ++dx) {
            const i32 cx = camCx + dx;
            const i32 cz = camCz + dz;
            if (chunks.contains(keyOf(cx, cz))) {
                continue;
            }
            wanted.push_back({ cx, cz, dx * dx + dz * dz });
        }
    }
    std::sort(wanted.begin(), wanted.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.dist2 < b.dist2;
              });
    u32 requests = 0;
    for (const Candidate& c : wanted) {
        if (requests >= kMaxRequestsPerFrame) {
            break;
        }
        const i32 cx = c.cx;
        const i32 cz = c.cz;
        chunks.emplace(keyOf(cx, cz), Chunk {});
        jobs->enqueue([sharedRef = shared, params, cx, cz,
                       gen = generation] {
            sharedRef->built.push(
                { cx, cz, gen, scatterProps(params, cx, cz) });
        });
        ++requests;
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

void VegetationSystem::buildCasterPipeline(rhi::Device& device,
                                           ShaderLibrary& shaders) {
    if (casterPipeline.id != 0) {
        device.destroyPipeline(casterPipeline);
    }
    casterPipeline = device.createPipeline(
        { .shader = shaders.get(kPropCasterShader),
          .vertexBuffers =
              { { .stride = sizeof(MeshVertex),
                  .attributes = { { .location = 0,
                                    .format = rhi::VertexFormat::F32x3,
                                    .offset = offsetof(MeshVertex, position) },
                                  { .location = 2,
                                    .format = rhi::VertexFormat::F32x2,
                                    .offset = offsetof(MeshVertex, uv) } } },
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
          .cull = rhi::CullMode::Back,
          .depthBias = 4.0f,
          .depthBiasSlope = 2.5f });
    casterShaderGeneration = shaders.generation(kPropCasterShader);
}

void VegetationSystem::refreshPipeline(rhi::Device& device,
                                       ShaderLibrary& shaders) {
    if (shaders.generation(kTreeShader) != shaderGeneration) {
        buildPipeline(device, shaders);
    }
    if (shaders.generation(kPropCasterShader) != casterShaderGeneration) {
        buildCasterPipeline(device, shaders);
    }
}

void VegetationSystem::draw(rhi::CommandBuffer& cmd,
                            rhi::BindGroupHandle frameBindGroup,
                            rhi::BindGroupHandle shadowBindGroup,
                            u32 variantLimit, const Vec3& cameraPos,
                            bool forceLowDetail, const Frustum* frustum,
                            const std::unordered_set<u64>* occluded) {
    // Frustum verdict per chunk, computed once (the variant-major loops
    // revisit every chunk per variant). Props overhang their chunk: pad XZ
    // by the canopy reach and the top by the tallest scaled tree.
    const auto chunkVisible = [&](u64 key, const Chunk& chunk) {
        if (occluded && occluded->contains(key)) {
            return false; // hidden behind a ridge (brick 26)
        }
        if (!frustum) {
            return true;
        }
        const f32 x0 = static_cast<f32>(static_cast<i32>(key >> 32)) *
                       TerrainSystem::kChunkSize;
        const f32 z0 = static_cast<f32>(static_cast<i32>(key & 0xffffffffu)) *
                       TerrainSystem::kChunkSize;
        return frustum->intersectsAabb(
            { x0 - 4.0f, chunk.minY - 1.0f, z0 - 4.0f },
            { x0 + TerrainSystem::kChunkSize + 4.0f, chunk.maxY + 14.0f,
              z0 + TerrainSystem::kChunkSize + 4.0f });
    };
    const bool culling = frustum != nullptr || occluded != nullptr;
    std::unordered_map<u64, bool> visible;
    if (culling) {
        visible.reserve(chunks.size());
        u32 drawnChunks = 0;
        for (const auto& [key, chunk] : chunks) {
            const bool v = chunk.resident && chunk.total > 0 &&
                           chunkVisible(key, chunk);
            visible.emplace(key, v);
            drawnChunks += v ? 1u : 0u;
        }
        lastDrawn = drawnChunks;
    }
    const auto culled = [&](u64 key) {
        if (!culling) {
            return false;
        }
        const auto it = visible.find(key);
        return it == visible.end() || !it->second;
    };

    cmd.setPipeline(pipeline);
    cmd.setBindGroup(0, frameBindGroup);
    if (shadowBindGroup.id != 0) {
        cmd.setBindGroup(2, shadowBindGroup);
    }
    // Canopy LOD pick, per chunk: high detail near the camera only (and
    // never in mirrored/downsampled passes). Variants without a low twin
    // (rocks, bushes, authored overrides) always use their main mesh.
    const i32 camCx = static_cast<i32>(
        std::floor(cameraPos.x / TerrainSystem::kChunkSize));
    const i32 camCz = static_cast<i32>(
        std::floor(cameraPos.z / TerrainSystem::kChunkSize));
    const auto lowDetail = [&](u64 key, const VariantMesh& mesh) {
        if (mesh.lowIndexCount == 0) {
            return false;
        }
        if (forceLowDetail) {
            return true;
        }
        const i32 cx = static_cast<i32>(key >> 32);
        const i32 cz = static_cast<i32>(key & 0xffffffffu);
        return std::max(std::abs(cx - camCx), std::abs(cz - camCz)) >
               kHighDetailRadius;
    };
    // Variant-major, split by LOD: bind each mesh level once, then one
    // instanced draw per chunk holding that variant (firstInstance =
    // offset into the chunk's variant-sorted buffer; needs baseInstance,
    // present on 4.6).
    for (u32 v = 0; v < variantLimit; ++v) {
        const VariantMesh& mesh = variantMeshes[v];
        for (const bool lowPass : { false, true }) {
            bool meshBound = false;
            for (const auto& [key, chunk] : chunks) {
                if (!chunk.resident || chunk.counts[v] == 0 ||
                    culled(key) || lowDetail(key, mesh) != lowPass) {
                    continue;
                }
                if (!meshBound) {
                    cmd.setVertexBuffer(0, lowPass ? mesh.lowVertexBuffer
                                                   : mesh.vertexBuffer);
                    cmd.setIndexBuffer(lowPass ? mesh.lowIndexBuffer
                                               : mesh.indexBuffer,
                                       rhi::IndexFormat::U32);
                    meshBound = true;
                }
                cmd.setVertexBuffer(1, chunk.instanceBuffer);
                cmd.drawIndexed(lowPass ? mesh.lowIndexCount
                                        : mesh.indexCount,
                                chunk.counts[v], 0, chunk.firstInstance[v]);
            }
            if (mesh.lowIndexCount == 0) {
                break; // single-LOD variant: one pass covers every chunk
            }
        }
    }
}

void VegetationSystem::drawDepth(rhi::CommandBuffer& cmd,
                                 rhi::BindGroupHandle frameBindGroup,
                                 rhi::BindGroupHandle casterBindGroup,
                                 const Vec3& cameraPos,
                                 i32 maxChunkDistance) {
    const i32 camCx = static_cast<i32>(
        std::floor(cameraPos.x / TerrainSystem::kChunkSize));
    const i32 camCz = static_cast<i32>(
        std::floor(cameraPos.z / TerrainSystem::kChunkSize));
    cmd.setPipeline(casterPipeline);
    cmd.setBindGroup(0, frameBindGroup);
    cmd.setBindGroup(1, casterBindGroup);
    for (u32 v = 0; v < kVariantCount; ++v) {
        bool meshBound = false;
        for (const auto& [key, chunk] : chunks) {
            if (!chunk.resident || chunk.counts[v] == 0) {
                continue;
            }
            const i32 cx = static_cast<i32>(key >> 32);
            const i32 cz = static_cast<i32>(key & 0xffffffffu);
            if (std::max(std::abs(cx - camCx), std::abs(cz - camCz)) >
                maxChunkDistance) {
                continue; // beyond the last shadow cascade
            }
            if (!meshBound) {
                // Casters always use the low-detail twin when there is
                // one: an 80-face lobe throws the same soft shadow as a
                // 320-face lobe, three cascades cheaper.
                const bool low = variantMeshes[v].lowIndexCount != 0;
                cmd.setVertexBuffer(0, low
                                           ? variantMeshes[v].lowVertexBuffer
                                           : variantMeshes[v].vertexBuffer);
                cmd.setIndexBuffer(low ? variantMeshes[v].lowIndexBuffer
                                       : variantMeshes[v].indexBuffer,
                                   rhi::IndexFormat::U32);
                meshBound = true;
            }
            const u32 indexCount = variantMeshes[v].lowIndexCount != 0
                                       ? variantMeshes[v].lowIndexCount
                                       : variantMeshes[v].indexCount;
            cmd.setVertexBuffer(1, chunk.instanceBuffer);
            cmd.drawIndexed(indexCount, chunk.counts[v], 0,
                            chunk.firstInstance[v]);
        }
    }
}

} // namespace render
