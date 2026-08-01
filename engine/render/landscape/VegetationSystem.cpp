#include "engine/render/landscape/VegetationSystem.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/Hash.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/render/MeshVertexLayout.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/render/landscape/TerrainSystem.hpp"
#include "engine/assets/VertexAoCache.hpp"
#include "engine/platform/Paths.hpp"
#include "engine/render/landscape/TreeGenerator.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {

constexpr const char* kTreeShader = "tree";
constexpr const char* kPropCasterShader = "shadow_prop";
// Realistic-scale trees: x8 height against the player,
// so 2x the candidate spacing = 1/4 the density — giant forests, not
// hedges of them.
constexpr f32 kTreeSpacing = 8.0f; // meters between scatter candidates

// hashU32 / HashRng now live in engine/core/Hash.hpp (shared scatter hash family).
using core::hashU32;
using core::HashRng;

// Bush clumps: medium-frequency blobs so shrubs come in family groups —
// and, crossed with the forest-edge factor, break its isoline into clusters
// instead of tracing it as a string.
f32 bushClumpMask(u32 seed, f32 x, f32 z) {
    const f32 blob = terrain::noise01(seed ^ 0x452821e6u, x / 13.0f,
                                      z / 13.0f);
    return glm::smoothstep(0.52f, 0.66f, blob);
}

} // namespace

// Forest belts: broad noise thresholded, so woods come as forests and
// clearings — not confetti. Public: FarTerrain raises and darkens its
// coarse mesh with the SAME mask so the distant forest fringe continues
// the real scatter past the vegetation ring.
f32 forestMask(u32 seed, f32 x, f32 z) {
    const f32 broad =
        terrain::noise01(seed ^ 0x3c6ef372u, x / 105.0f, z / 105.0f);
    const f32 detail =
        terrain::noise01(seed ^ 0xa54ff53au, x / 28.0f, z / 28.0f);
    return glm::smoothstep(0.46f, 0.58f, broad * 0.78f + detail * 0.22f);
}

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
            // Density halving done on the acceptance rather than the
            // spacing so the grid keeps its resolution (spacing x
            // sqrt(2) would truncate).
            if (forest < 0.05f || rng.next() >= forest * 0.475f) {
                continue;
            }
            const f32 h = terrain::height(params, x, z);
            const Vec3 n = terrain::normal(params, x, z);
            const f32 slope = 1.0f - n.y;
            // Treeline scaled with the terrain amplitudes.
            if (h < params.seaLevel + 3.0f || h > 138.0f || slope > 0.22f) {
                continue;
            }
            // Sink slightly so leaning trunks never float on slopes; the
            // offset follows the scale (their footprint is meters wide).
            place(0, VegetationSystem::kTreeVariants, rng, x, h - 0.9f, z,
                  VegetationSystem::kTreeScaleMin,
                  VegetationSystem::kTreeScaleMax, 880.0f);
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
            const auto weights =
                terrain::materialWeightsAt(params, x, z, h, n);
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
            if (terrain::materialWeightsAt(params, x, z, h, n).grass <
                0.65f) {
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

u32 VegetationSystem::InstancePool::alloc(u32 size) {
    for (size_t i = 0; i < freeBlocks.size(); ++i) {
        if (freeBlocks[i].size < size) {
            continue; // first fit
        }
        const u32 offset = freeBlocks[i].offset;
        if (freeBlocks[i].size == size) {
            freeBlocks.erase(freeBlocks.begin() +
                             static_cast<std::ptrdiff_t>(i));
        } else {
            freeBlocks[i].offset += size;
            freeBlocks[i].size -= size;
        }
        return offset;
    }
    return kNoOffset;
}

void VegetationSystem::InstancePool::tick() {
    for (const Block& cooled : cooling[1]) {
        // Sorted insert + coalesce with both neighbors.
        auto it = std::lower_bound(
            freeBlocks.begin(), freeBlocks.end(), cooled,
            [](const Block& a, const Block& b) {
                return a.offset < b.offset;
            });
        it = freeBlocks.insert(it, cooled);
        if (it + 1 != freeBlocks.end() &&
            it->offset + it->size == (it + 1)->offset) {
            it->size += (it + 1)->size;
            it = freeBlocks.erase(it + 1) - 1;
        }
        if (it != freeBlocks.begin() &&
            (it - 1)->offset + (it - 1)->size == it->offset) {
            (it - 1)->size += it->size;
            freeBlocks.erase(it);
        }
    }
    cooling[1] = std::move(cooling[0]);
    cooling[0].clear();
}

void VegetationSystem::create(rhi::Device& device, ShaderLibrary& shaders,
                              core::JobSystem& jobSystem, u32 terrainSeed) {
    streamer.create(jobSystem);
    meshSeed = terrainSeed;
    instancePool.buffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = u64(InstancePool::kCapacity) * sizeof(Instance) },
        nullptr) };
    instancePool.freeBlocks = { { 0, InstancePool::kCapacity } };
    createVariantMeshes(device, terrainSeed);
    rebuildLeafMask(device);
    shaders.load(kTreeShader, { { "FrameUbo", 0 } },
                 { { "uLeafMask", 0 }, { "uShadowMap", 1 } });
    buildPipeline(device, shaders);
    shaders.load(kPropCasterShader, { { "FrameUbo", 0 }, { "ShadowUbo", 1 } },
                 { { "uLeafMask", 0 } });
    buildCasterPipeline(device, shaders);
}

void VegetationSystem::rebuildLeafMask(rhi::Device& device) {
    constexpr u32 kMaskSize = 256;
    // Full mip chain when the device can fill it — alpha-tested cards
    // shrink to a few pixels at the ring edge; base-level-only sampling
    // there is pure shimmer.
    const bool mips = device.caps().mipmapGeneration;
    const u32 mipLevels =
        mips ? 1 + static_cast<u32>(std::log2(static_cast<f32>(kMaskSize)))
             : 1;
    const vector<u8> pixels =
        generateLeafMaskPixels(kMaskSize, meshSeed, colonizedTreeParams);
    leafMask = { device, device.createTexture(
        { .width = kMaskSize,
          .height = kMaskSize,
          .mipLevels = mipLevels,
          .format = rhi::TextureFormat::RGBA8,
          .filter = rhi::FilterMode::Linear },
        pixels.data()) };
    if (mips) {
        device.generateMipmaps(leafMask.get());
    }
    if (leafMaskSampler.get().id == 0) {
        leafMaskSampler = { device,
                            device.createSampler(
                                { .mipmapFilter = mips }) }; // linear clamp
    }
    leafMaskGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = leafMask.get(),
                         .sampler = leafMaskSampler.get() } } }) };
}

void VegetationSystem::createVariantMeshes(rhi::Device& device,
                                           u32 terrainSeed) {
    // Ambient grounding is BAKED into the vertex
    // colors — canopy interiors and rock creases darken with zero
    // runtime cost. Content-keyed DISK CACHE (same store as the glTF
    // bakes): these 17 synchronous bakes cost ~a minute in an
    // unoptimized Debug build — once per geometry now, then loads.
    // [cpp-tuning] strengths below.
    const std::filesystem::path aoCacheDir =
        platform::executableDir() / "data" / "cache" / "ao";
    const auto baked = [&](MeshData mesh, f32 strength) {
        assets::applyContentKeyedVertexAo(mesh, aoCacheDir, strength);
        return mesh;
    };
    for (u32 i = 0; i < kVariantCount; ++i) {
        if (const auto it = meshOverrides.find(i);
            it != meshOverrides.end()) {
            uploadVariantMesh(device, i, baked(it->second, 0.55f));
            continue;
        }
        const u32 seed = hashU32(terrainSeed) + i * 977u;
        if (i < kFirstRock) {
            // EXPERIMENT A/B (feature/space-colonization-trees): the
            // Runions/SDF-card generator swaps in for all three levels.
            const auto tree = [&](u32 lod) {
                return colonizationTrees
                           ? generateColonizedTree(seed, lod,
                                                   colonizedTreeParams)
                           : generateTree(seed, lod, lobeTreeParams);
            };
            uploadVariantMesh(device, i, baked(tree(2), 0.6f));
            uploadLowDetailMesh(device, i, baked(tree(1), 0.6f));
            // Bare-icosahedron lobes (~150 tris/tree) for the far
            // ring — same seed, same composition, facets invisible there.
            uploadUltraDetailMesh(device, i, baked(tree(0), 0.6f));
            if (colonizationTrees) {
                // Far-cascade caster: solid metaball blobs, no AO bake
                // (depth-only) — see generateColonizedTreeShadowProxy.
                uploadShadowProxyMesh(
                    device, i,
                    generateColonizedTreeShadowProxy(seed,
                                                     colonizedTreeParams));
            }
        } else if (i < kFirstBush) {
            uploadVariantMesh(device, i, baked(generateRock(seed), 0.5f));
        } else {
            uploadVariantMesh(device, i, baked(generateBush(seed), 0.55f));
        }
    }
}

VegetationSystem::TreeSilhouette VegetationSystem::treeSilhouette() const {
    Vec3 sum { 0.0f };
    u32 count = 0;
    for (const Vec3& bounds : treeBounds) {
        if (bounds.x > 0.1f) {
            sum += bounds;
            ++count;
        }
    }
    if (count == 0) {
        return {}; // defaults until the first variant lands
    }
    const Vec3 mean = sum / static_cast<f32>(count);
    const f32 scale = (kTreeScaleMin + kTreeScaleMax) * 0.5f;
    return { mean.x * scale,
             glm::clamp(2.0f * mean.y / glm::max(mean.x, 0.1f), 0.4f,
                        1.4f),
             glm::clamp(mean.z / glm::max(mean.x, 0.1f), 0.05f, 0.7f) };
}

void VegetationSystem::uploadVariantMesh(rhi::Device& device, u32 variant,
                                         const MeshData& mesh) {
    if (variant < kTreeVariants && !mesh.vertices.empty()) {
        // Silhouette measurement (see TreeSilhouette): height, widest
        // radial extent, and where the crown starts (first height with
        // real width — everything below is bare trunk).
        f32 maxY = 0.0f;
        f32 maxR = 0.0f;
        for (const MeshVertex& v : mesh.vertices) {
            maxY = glm::max(maxY, v.position.y);
            maxR = glm::max(maxR, glm::length(Vec2 { v.position.x,
                                                     v.position.z }));
        }
        f32 crownStart = maxY;
        for (const MeshVertex& v : mesh.vertices) {
            if (glm::length(Vec2 { v.position.x, v.position.z }) >
                maxR * 0.35f) {
                crownStart = glm::min(crownStart, v.position.y);
            }
        }
        treeBounds[variant] = { maxY, maxR, crownStart };
    }
    variantMeshes[variant].indexCount =
        static_cast<u32>(mesh.indices.size());
    variantMeshes[variant].vertexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = mesh.vertices.size() * sizeof(MeshVertex) },
        mesh.vertices.data()) };
    variantMeshes[variant].indexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = mesh.indices.size() * sizeof(u32) },
        mesh.indices.data()) };
}

void VegetationSystem::uploadLowDetailMesh(rhi::Device& device, u32 variant,
                                           const MeshData& mesh) {
    variantMeshes[variant].lowIndexCount =
        static_cast<u32>(mesh.indices.size());
    variantMeshes[variant].lowVertexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = mesh.vertices.size() * sizeof(MeshVertex) },
        mesh.vertices.data()) };
    variantMeshes[variant].lowIndexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = mesh.indices.size() * sizeof(u32) },
        mesh.indices.data()) };
}

void VegetationSystem::uploadUltraDetailMesh(rhi::Device& device, u32 variant,
                                             const MeshData& mesh) {
    variantMeshes[variant].ultraIndexCount =
        static_cast<u32>(mesh.indices.size());
    variantMeshes[variant].ultraVertexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = mesh.vertices.size() * sizeof(MeshVertex) },
        mesh.vertices.data()) };
    variantMeshes[variant].ultraIndexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = mesh.indices.size() * sizeof(u32) },
        mesh.indices.data()) };
}

void VegetationSystem::uploadShadowProxyMesh(rhi::Device& device,
                                             u32 variant,
                                             const MeshData& mesh) {
    variantMeshes[variant].casterIndexCount =
        static_cast<u32>(mesh.indices.size());
    variantMeshes[variant].casterVertexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = mesh.vertices.size() * sizeof(MeshVertex) },
        mesh.vertices.data()) };
    variantMeshes[variant].casterIndexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = mesh.indices.size() * sizeof(u32) },
        mesh.indices.data()) };
}

void VegetationSystem::overrideVariantMesh(rhi::Device& device, u32 variant,
                                           MeshData mesh) {
    if (variant >= kVariantCount || mesh.vertices.empty() ||
        mesh.indices.empty()) {
        return;
    }
    // U3-7: the reset frees both detail levels through their wrappers
    // (authored meshes come as ONE detail level — low twin stays empty).
    variantMeshes[variant] = {};
    uploadVariantMesh(device, variant, mesh);
    meshOverrides[variant] = std::move(mesh);
}

void VegetationSystem::destroyVariantMeshes(rhi::Device& device) {
    (void)device; // U3-7: assignment frees through the wrappers
    for (VariantMesh& variant : variantMeshes) {
        variant = {};
    }
}

void VegetationSystem::reseedVariantMeshesAsync(core::JobSystem& jobs,
                                                u32 seed) {
    reseedJobs = &jobs;
    if (reseedJob) {
        // Coalesce: the in-flight job lands first, then relaunches with
        // the LATEST params (knob storms collapse into two passes).
        reseedQueued = true;
        reseedQueuedSeed = seed;
        return;
    }
    meshSeed = seed;
    auto job = std::make_shared<ReseedJob>();
    job->seed = seed;
    job->colonization = colonizationTrees;
    job->lobes = lobeTreeParams;
    job->colonized = colonizedTreeParams;
    job->aoCacheDir =
        platform::executableDir() / "data" / "cache" / "ao";
    job->total = kTreeVariants * (job->colonization ? 4u : 3u);
    reseedJob = job;
    jobs.enqueue([job] {
        // Pure CPU (mesh generation + content-keyed AO bake) — the
        // MeshCache decode-worker pattern; only the sptr is captured.
        const auto baked = [&](MeshData mesh) {
            assets::applyContentKeyedVertexAo(mesh, job->aoCacheDir, 0.6f);
            return mesh;
        };
        for (u32 i = 0; i < kTreeVariants; ++i) {
            const u32 variantSeed = hashU32(job->seed) + i * 977u;
            for (u32 lod = 0; lod < 3; ++lod) {
                job->lods[i][lod] = baked(
                    job->colonization
                        ? generateColonizedTree(variantSeed, lod,
                                                job->colonized)
                        : generateTree(variantSeed, lod, job->lobes));
                job->completed.fetch_add(1, std::memory_order_release);
            }
            if (job->colonization) {
                job->casters[i] = generateColonizedTreeShadowProxy(
                    variantSeed, job->colonized);
                job->completed.fetch_add(1, std::memory_order_release);
            }
        }
        job->done.store(true, std::memory_order_release);
    });
}

f32 VegetationSystem::reseedProgress() const {
    if (!reseedJob) {
        return 1.0f;
    }
    return static_cast<f32>(
               reseedJob->completed.load(std::memory_order_acquire)) /
           static_cast<f32>(glm::max(reseedJob->total, 1u));
}

void VegetationSystem::pumpReseed(rhi::Device& device) {
    if (!reseedJob || !reseedJob->done.load(std::memory_order_acquire)) {
        return;
    }
    const sptr<ReseedJob> job = std::move(reseedJob);
    for (u32 i = 0; i < kTreeVariants; ++i) {
        if (meshOverrides.contains(i)) {
            continue; // authored meshes keep their single level
        }
        variantMeshes[i] = {}; // clears a stale caster on an algo switch
        uploadVariantMesh(device, i, job->lods[i][2]);
        uploadLowDetailMesh(device, i, job->lods[i][1]);
        uploadUltraDetailMesh(device, i, job->lods[i][0]);
        if (job->colonization) {
            uploadShadowProxyMesh(device, i, job->casters[i]);
        }
    }
    rebuildLeafMask(device); // its knobs ride the same panel
    if (reseedQueued) {
        reseedQueued = false;
        if (reseedJobs != nullptr) {
            reseedVariantMeshesAsync(*reseedJobs, reseedQueuedSeed);
        }
    }
}

void VegetationSystem::setShowcase(rhi::Device& device,
                                   const vector<Instance>& list) {
    showcaseInstances.reset();
    showcaseCount = static_cast<u32>(list.size());
    if (list.empty()) {
        return;
    }
    showcaseInstances = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = list.size() * sizeof(Instance) },
        list.data()) };
}

void VegetationSystem::destroy(rhi::Device& device) {
    streamer.invalidateAll([](Chunk&) {});
    instancePool = {};
    instances = 0;
    showcaseInstances.reset();
    showcaseCount = 0;
    reseedJob.reset(); // a worker still running keeps its own copy
    reseedQueued = false;
    pipeline.reset();
    casterPipeline.reset();
    leafMaskGroup.reset();
    leafMaskSampler.reset();
    leafMask.reset();
    destroyVariantMeshes(device);
}

void VegetationSystem::regenerate(rhi::Device& device, u32 terrainSeed) {
    streamer.invalidateAll([&](Chunk& chunk) {
        if (chunk.resident && chunk.total > 0 &&
            chunk.poolOffset != kNoOffset) {
            instancePool.free(chunk.poolOffset, chunk.total);
        }
    });
    instances = 0;
    meshSeed = terrainSeed;
    destroyVariantMeshes(device);
    createVariantMeshes(device, terrainSeed);
}

void VegetationSystem::invalidateChunks(rhi::Device& device,
                                        const vector<u64>& keys) {
    (void)device;
    // The shared variant meshes are height-independent — only per-chunk scatter
    // is dropped so props re-seat on the new terrain.
    for (const u64 key : keys) {
        const auto it = streamer.chunks.find(key);
        if (it == streamer.chunks.end() || !it->second.resident) {
            continue; // missing, or still streaming in (no stale swap)
        }
        if (it->second.total > 0 && it->second.poolOffset != kNoOffset) {
            instancePool.free(it->second.poolOffset, it->second.total);
        }
        instances -= it->second.total;
        // update() re-requests + re-scatters with new heights.
        streamer.chunks.erase(it);
    }
}

void VegetationSystem::update(rhi::Device& device, const TerrainParams& params,
                              const Vec3& cameraPos) {
    frameIndices = 0; // the frame's draw*() calls sum into these
    frameHighInstances = 0;
    frameLowInstances = 0;
    frameUltraInstances = 0;
    pumpReseed(device); // async reseed landing point (main thread)
    if (showcaseCount != 0) {
        return; // showcase replaces the streamed scatter entirely
    }
    instancePool.tick(); // two-frame-cooled blocks become reusable
    // Budgeted uploads (U3-1: ring mechanics in ChunkStreamer; this lambda
    // is the vegetation-specific accept — variant packing + GPU upload
    // into the pooled instance buffer).
    streamer.pump(kMaxUploadsPerFrame, 0.0, [&](u64 key, auto& built) {
        const auto it = streamer.chunks.find(key);
        if (it == streamer.chunks.end() || it->second.resident) {
            return false;
        }
        Chunk& chunk = it->second;
        vector<Instance> packed;
        chunk.giProps.clear();
        for (u32 v = 0; v < kVariantCount; ++v) {
            chunk.firstInstance[v] = static_cast<u32>(packed.size());
            chunk.counts[v] = static_cast<u32>(built.payload[v].size());
            packed.insert(packed.end(), built.payload[v].begin(),
                          built.payload[v].end());
            // The compact CPU copy the GI injection boxes.
            const u8 kind = v < kFirstRock ? 0 : v < kFirstBush ? 1 : 2;
            for (const Instance& instance : built.payload[v]) {
                chunk.giProps.push_back(
                    { Vec3 { instance.positionScale },
                      instance.positionScale.w, kind });
            }
        }
        chunk.total = static_cast<u32>(packed.size());
        if (chunk.total > 0) {
            const u32 offset = instancePool.alloc(chunk.total);
            if (offset == kNoOffset) {
                LOG_WARN("VegetationSystem: instance pool full — chunk "
                         "scatter dropped");
                streamer.chunks.erase(it); // re-detected and re-requested
                return false;
            }
            device.updateBuffer(instancePool.buffer.get(), packed.data(),
                                packed.size() * sizeof(Instance),
                                u64(offset) * sizeof(Instance));
            chunk.poolOffset = offset;
            chunk.minY = packed[0].positionScale.y;
            chunk.maxY = chunk.minY;
            for (const Instance& instance : packed) {
                chunk.minY = glm::min(chunk.minY, instance.positionScale.y);
                chunk.maxY = glm::max(chunk.maxY, instance.positionScale.y);
            }
        }
        chunk.resident = true;
        instances += chunk.total;
        return true;
    });

    // (giProps live in the Chunk: eviction frees them with it.)

    // Request missing chunks — nearest first, budgeted (the rest is
    // re-detected next frame; the state IS the queue). See TerrainSystem:
    // the unbudgeted ring edge was part of the fast-travel stutter.
    const i32 camCx = chunkCoordOf(cameraPos.x, TerrainSystem::kChunkSize);
    const i32 camCz = chunkCoordOf(cameraPos.z, TerrainSystem::kChunkSize);
    streamer.requestMissing(
        camCx, camCz, viewRadius, kMaxRequestsPerFrame,
        [&](i32 cx, i32 cz, i32, i32) {
            return !streamer.chunks.contains(chunkKey(cx, cz));
        },
        [&](i32 cx, i32 cz, i32, i32) {
            streamer.chunks.emplace(chunkKey(cx, cz), Chunk {});
            streamer.enqueueBuild(cx, cz, [params, cx, cz] {
                return scatterProps(params, cx, cz);
            });
        });

    // Evict beyond hysteresis.
    streamer.evictFar(camCx, camCz, viewRadius + 1, [&](Chunk& chunk) {
        if (chunk.resident) {
            if (chunk.total > 0 && chunk.poolOffset != kNoOffset) {
                instancePool.free(chunk.poolOffset, chunk.total);
            }
            instances -= chunk.total;
        }
    });
}

void VegetationSystem::buildPipeline(rhi::Device& device,
                                     ShaderLibrary& shaders) {
    pipeline = { device, device.createPipeline( // U3-7: frees the old one
        { .shader = shaders.get(kTreeShader),
          .vertexBuffers =
              { meshVertexLayout(), // U3-5 (the caster keeps its own
                                    // position+uv layout — sway weights)
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
                     .compare = rhi::CompareFunc::Greater }, // reversed-Z
          .cull = rhi::CullMode::Back }) };
    shaderGeneration = shaders.generation(kTreeShader);
}

void VegetationSystem::buildCasterPipeline(rhi::Device& device,
                                           ShaderLibrary& shaders) {
    casterPipeline = { device, device.createPipeline( // U3-7
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
          .depthBiasSlope = 2.5f }) };
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
            return false; // hidden behind a ridge (ChunkOcclusion)
        }
        if (!frustum) {
            return true;
        }
        const f32 x0 =
            static_cast<f32>(chunkKeyCx(key)) * TerrainSystem::kChunkSize;
        const f32 z0 =
            static_cast<f32>(chunkKeyCz(key)) * TerrainSystem::kChunkSize;
        return frustum->intersectsAabb(
            { x0 - kPropPadXz, chunk.minY - 1.0f, z0 - kPropPadXz },
            { x0 + TerrainSystem::kChunkSize + kPropPadXz,
              chunk.maxY + kPropPadY,
              z0 + TerrainSystem::kChunkSize + kPropPadXz });
    };
    const bool culling = frustum != nullptr || occluded != nullptr;
    std::unordered_map<u64, bool> visible;
    if (culling) {
        visible.reserve(streamer.chunks.size());
        u32 drawnChunks = 0;
        for (const auto& [key, chunk] : streamer.chunks) {
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
    cmd.setBindGroup(1, leafMaskGroup);
    if (shadowBindGroup.id != 0) {
        cmd.setBindGroup(2, shadowBindGroup);
    }
    // Showcase mode: the explicit instances with variant 0's full-detail
    // mesh, nothing else (tool scenes — the streamer holds no chunks).
    if (showcaseCount != 0) {
        const VariantMesh& mesh = variantMeshes[0];
        cmd.setVertexBuffer(0, mesh.vertexBuffer.get());
        cmd.setIndexBuffer(mesh.indexBuffer.get(), rhi::IndexFormat::U32);
        cmd.setVertexBuffer(1, showcaseInstances.get());
        cmd.drawIndexed(mesh.indexCount, showcaseCount, 0, 0);
        frameIndices += mesh.indexCount * showcaseCount;
        frameHighInstances += showcaseCount;
        return;
    }
    // Canopy LOD pick, per chunk — THREE levels: 320-face lobes
    // near, 80-face twins mid, 20-face ultra beyond lowDetailRadius (and
    // always in mirrored/downsampled passes). Variants without twins
    // (rocks, bushes, authored overrides) always use their main mesh.
    const i32 camCx = chunkCoordOf(cameraPos.x, TerrainSystem::kChunkSize);
    const i32 camCz = chunkCoordOf(cameraPos.z, TerrainSystem::kChunkSize);
    const auto detailLevel = [&](u64 key, const VariantMesh& mesh) -> u32 {
        if (mesh.lowIndexCount == 0) {
            return 0u;
        }
        if (forceLowDetail) {
            return mesh.ultraIndexCount != 0 ? 2u : 1u;
        }
        const i32 cx = chunkKeyCx(key);
        const i32 cz = chunkKeyCz(key);
        const i32 cheb =
            std::max(std::abs(cx - camCx), std::abs(cz - camCz));
        if (cheb <= highDetailRadius) {
            return 0u;
        }
        if (mesh.ultraIndexCount == 0 || cheb <= lowDetailRadius) {
            return 1u;
        }
        return 2u;
    };
    // Variant-major, split by LOD: bind each mesh level once, then one
    // instanced draw per chunk holding that variant (firstInstance =
    // offset into the chunk's variant-sorted buffer; needs baseInstance,
    // present on 4.6).
    for (u32 v = 0; v < variantLimit; ++v) {
        const VariantMesh& mesh = variantMeshes[v];
        const u32 levels = mesh.lowIndexCount == 0
                               ? 1u
                               : (mesh.ultraIndexCount == 0 ? 2u : 3u);
        for (u32 level = 0; level < levels; ++level) {
            const rhi::BufferHandle vb =
                level == 0 ? mesh.vertexBuffer.get()
                : level == 1 ? mesh.lowVertexBuffer.get()
                             : mesh.ultraVertexBuffer.get();
            const rhi::BufferHandle ib =
                level == 0 ? mesh.indexBuffer.get()
                : level == 1 ? mesh.lowIndexBuffer.get()
                             : mesh.ultraIndexBuffer.get();
            const u32 indexCount = level == 0 ? mesh.indexCount
                                   : level == 1 ? mesh.lowIndexCount
                                                : mesh.ultraIndexCount;
            bool meshBound = false;
            for (const auto& [key, chunk] : streamer.chunks) {
                if (!chunk.resident || chunk.counts[v] == 0 ||
                    culled(key) || detailLevel(key, mesh) != level) {
                    continue;
                }
                if (!meshBound) {
                    cmd.setVertexBuffer(0, vb);
                    cmd.setIndexBuffer(ib, rhi::IndexFormat::U32);
                    meshBound = true;
                }
                cmd.setVertexBuffer(1, instancePool.buffer.get(),
                                    u64(chunk.poolOffset) *
                                        sizeof(Instance));
                cmd.drawIndexed(indexCount, chunk.counts[v], 0,
                                chunk.firstInstance[v]);
                frameIndices += indexCount * chunk.counts[v];
                (level == 0   ? frameHighInstances
                 : level == 1 ? frameLowInstances
                              : frameUltraInstances) += chunk.counts[v];
            }
        }
    }
}

void VegetationSystem::collectDrawCandidates(
    vector<GpuOcclusion::Candidate>& out, const Vec3& cameraPos) const {
    if (showcaseCount != 0) {
        return; // showcase renders through the legacy path only
    }
    const i32 camCx = chunkCoordOf(cameraPos.x, TerrainSystem::kChunkSize);
    const i32 camCz = chunkCoordOf(cameraPos.z, TerrainSystem::kChunkSize);
    for (const auto& [key, chunk] : streamer.chunks) {
        if (!chunk.resident || chunk.total == 0 ||
            chunk.poolOffset == kNoOffset) {
            continue;
        }
        const i32 cx = chunkKeyCx(key);
        const i32 cz = chunkKeyCz(key);
        const i32 cheb =
            std::max(std::abs(cx - camCx), std::abs(cz - camCz));
        // Same padded AABB as draw()'s chunkVisible.
        const f32 x0 = static_cast<f32>(cx) * TerrainSystem::kChunkSize;
        const f32 z0 = static_cast<f32>(cz) * TerrainSystem::kChunkSize;
        const Vec3 lo { x0 - kPropPadXz, chunk.minY - 1.0f,
                        z0 - kPropPadXz };
        const Vec3 hi { x0 + TerrainSystem::kChunkSize + kPropPadXz,
                        chunk.maxY + kPropPadY,
                        z0 + TerrainSystem::kChunkSize + kPropPadXz };
        for (u32 v = 0; v < kVariantCount; ++v) {
            if (chunk.counts[v] == 0) {
                continue;
            }
            // Same level pick as draw()'s detailLevel lambda.
            const VariantMesh& mesh = variantMeshes[v];
            u32 level = 0;
            if (mesh.lowIndexCount != 0 && cheb > highDetailRadius) {
                level = mesh.ultraIndexCount == 0 || cheb <= lowDetailRadius
                            ? 1u
                            : 2u;
            }
            const u32 indexCount = level == 0   ? mesh.indexCount
                                   : level == 1 ? mesh.lowIndexCount
                                                : mesh.ultraIndexCount;
            out.push_back({ lo, hi, kGroupBase + v * 3 + level, indexCount,
                            0, chunk.counts[v],
                            chunk.poolOffset + chunk.firstInstance[v] });
        }
    }
}

void VegetationSystem::drawIndirect(rhi::CommandBuffer& cmd,
                                    rhi::BindGroupHandle frameBindGroup,
                                    rhi::BindGroupHandle shadowBindGroup,
                                    rhi::BufferHandle commands,
                                    const u32* groupFirst,
                                    const u32* groupCount) {
    cmd.setPipeline(pipeline);
    cmd.setBindGroup(0, frameBindGroup);
    cmd.setBindGroup(1, leafMaskGroup);
    if (shadowBindGroup.id != 0) {
        cmd.setBindGroup(2, shadowBindGroup);
    }
    // One pooled instance buffer for every batch; each command's
    // firstInstance addresses its chunk slice.
    cmd.setVertexBuffer(1, instancePool.buffer.get());
    constexpr u32 kStride = sizeof(rhi::DrawIndexedIndirectCommand);
    for (u32 v = 0; v < kVariantCount; ++v) {
        const VariantMesh& mesh = variantMeshes[v];
        const u32 levels = mesh.lowIndexCount == 0
                               ? 1u
                               : (mesh.ultraIndexCount == 0 ? 2u : 3u);
        for (u32 level = 0; level < levels; ++level) {
            const u32 group = kGroupBase + v * 3 + level;
            if (groupCount[group] == 0) {
                continue;
            }
            cmd.setVertexBuffer(0, level == 0 ? mesh.vertexBuffer.get()
                                   : level == 1
                                       ? mesh.lowVertexBuffer.get()
                                       : mesh.ultraVertexBuffer.get());
            cmd.setIndexBuffer(level == 0   ? mesh.indexBuffer.get()
                               : level == 1 ? mesh.lowIndexBuffer.get()
                                            : mesh.ultraIndexBuffer.get(),
                               rhi::IndexFormat::U32);
            cmd.drawIndexedIndirect(commands,
                                    u64(groupFirst[group]) * kStride,
                                    groupCount[group], kStride);
        }
    }
    // (The per-level instance counters can't be known CPU-side on this
    // path — the panel's dissection belongs to the legacy A/B.)
}

void VegetationSystem::drawDepth(rhi::CommandBuffer& cmd,
                                 rhi::BindGroupHandle frameBindGroup,
                                 rhi::BindGroupHandle casterBindGroup,
                                 const Vec3& cameraPos,
                                 i32 maxChunkDistance,
                                 const Frustum* frustum, bool ultraDetail) {
    const i32 camCx = chunkCoordOf(cameraPos.x, TerrainSystem::kChunkSize);
    const i32 camCz = chunkCoordOf(cameraPos.z, TerrainSystem::kChunkSize);
    cmd.setPipeline(casterPipeline);
    cmd.setBindGroup(0, frameBindGroup);
    cmd.setBindGroup(1, casterBindGroup);
    cmd.setBindGroup(2, leafMaskGroup);
    // Showcase mode: the explicit instances cast with the full-detail
    // mesh — one tree, no LOD/proxy economics needed.
    if (showcaseCount != 0) {
        const VariantMesh& mesh = variantMeshes[0];
        cmd.setVertexBuffer(0, mesh.vertexBuffer.get());
        cmd.setIndexBuffer(mesh.indexBuffer.get(), rhi::IndexFormat::U32);
        cmd.setVertexBuffer(1, showcaseInstances.get());
        cmd.drawIndexed(mesh.indexCount, showcaseCount, 0, 0);
        frameIndices += mesh.indexCount * showcaseCount;
        return;
    }
    for (u32 v = 0; v < kVariantCount; ++v) {
        bool meshBound = false;
        for (const auto& [key, chunk] : streamer.chunks) {
            if (!chunk.resident || chunk.counts[v] == 0) {
                continue;
            }
            const i32 cx = chunkKeyCx(key);
            const i32 cz = chunkKeyCz(key);
            if (std::max(std::abs(cx - camCx), std::abs(cz - camCz)) >
                maxChunkDistance) {
                continue; // beyond the last shadow cascade
            }
            if (frustum != nullptr) {
                // Same AABB convention as draw() (kPropPad*: canopy
                // overhang in XZ, tallest scaled tree in Y).
                const f32 x0 =
                    static_cast<f32>(cx) * TerrainSystem::kChunkSize;
                const f32 z0 =
                    static_cast<f32>(cz) * TerrainSystem::kChunkSize;
                if (!frustum->intersectsAabb(
                        { x0 - kPropPadXz, chunk.minY - 1.0f,
                          z0 - kPropPadXz },
                        { x0 + TerrainSystem::kChunkSize + kPropPadXz,
                          chunk.maxY + kPropPadY,
                          z0 + TerrainSystem::kChunkSize + kPropPadXz })) {
                    continue; // outside this cascade's ortho volume
                }
            }
            // Casters use the cheapest twin the cascade tolerates: the
            // 80-face lobe throws the same soft shadow as a 320-face one;
            // the far cascades (ultraDetail) prefer the SOLID shadow
            // proxy (metaball blobs — no cards, no cutout), else the
            // 20-face level — their texels are meters wide anyway.
            const VariantMesh& mesh = variantMeshes[v];
            const bool proxy = ultraDetail && mesh.casterIndexCount != 0;
            const bool ultra =
                !proxy && ultraDetail && mesh.ultraIndexCount != 0;
            const bool low = !proxy && !ultra && mesh.lowIndexCount != 0;
            if (!meshBound) {
                cmd.setVertexBuffer(0,
                                    proxy   ? mesh.casterVertexBuffer.get()
                                    : ultra ? mesh.ultraVertexBuffer.get()
                                    : low   ? mesh.lowVertexBuffer.get()
                                            : mesh.vertexBuffer.get());
                cmd.setIndexBuffer(proxy   ? mesh.casterIndexBuffer.get()
                                   : ultra ? mesh.ultraIndexBuffer.get()
                                   : low   ? mesh.lowIndexBuffer.get()
                                           : mesh.indexBuffer.get(),
                                   rhi::IndexFormat::U32);
                meshBound = true;
            }
            const u32 indexCount = proxy   ? mesh.casterIndexCount
                                   : ultra ? mesh.ultraIndexCount
                                   : low   ? mesh.lowIndexCount
                                           : mesh.indexCount;
            cmd.setVertexBuffer(1, instancePool.buffer.get(),
                                u64(chunk.poolOffset) * sizeof(Instance));
            cmd.drawIndexed(indexCount, chunk.counts[v], 0,
                            chunk.firstInstance[v]);
            frameIndices += indexCount * chunk.counts[v];
            (proxy || ultra ? frameUltraInstances : frameLowInstances) +=
                chunk.counts[v];
        }
    }
}

void VegetationSystem::collectGiProps(const Vec3& center, f32 halfSpan,
                                      vector<GiProp>& out,
                                      size_t maxProps) const {
    // Nearest chunks first so the box cap keeps the props that matter.
    struct Near {
        f32 distance;
        const Chunk* chunk;
    };
    vector<Near> near;
    for (const auto& [key, chunk] : streamer.chunks) {
        if (!chunk.resident || chunk.giProps.empty()) {
            continue;
        }
        const f32 cx = (static_cast<f32>(chunkKeyCx(key)) + 0.5f) *
                       TerrainSystem::kChunkSize;
        const f32 cz = (static_cast<f32>(chunkKeyCz(key)) + 0.5f) *
                       TerrainSystem::kChunkSize;
        const f32 distance = glm::max(glm::abs(cx - center.x),
                                      glm::abs(cz - center.z));
        if (distance > halfSpan + TerrainSystem::kChunkSize) {
            continue;
        }
        near.push_back({ distance, &chunk });
    }
    std::sort(near.begin(), near.end(),
              [](const Near& a, const Near& b) {
                  return a.distance < b.distance;
              });
    for (const Near& entry : near) {
        for (const GiProp& prop : entry.chunk->giProps) {
            if (out.size() >= maxProps) {
                return;
            }
            if (glm::max(glm::abs(prop.position.x - center.x),
                         glm::abs(prop.position.z - center.z)) <= halfSpan) {
                out.push_back(prop);
            }
        }
    }
}

} // namespace render
