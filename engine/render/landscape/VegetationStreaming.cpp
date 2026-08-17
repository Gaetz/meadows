// Lifecycle + streaming: instance pool, create/destroy/regenerate,
// chunk invalidation and the per-frame update (ring requests, scatter
// jobs, uploads). Split from VegetationSystem.cpp (one TU per trade).
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
                 { { "uLeafMask", 0 },
                   { "uShadowMap", 1 },
                   { "uPropNormal", 12 },
                   { "uTerrainShade0", 4 },
                   { "uBark", 13 },
                   { "uBarkNrm", 14 } });
    buildPipeline(device, shaders);
    shaders.load(kPropCasterShader, { { "FrameUbo", 0 }, { "ShadowUbo", 1 } },
                 { { "uLeafMask", 0 } });
    buildCasterPipeline(device, shaders);
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
        if (it == streamer.chunks.end()) {
            continue;
        }
        if (!it->second.resident) {
            // Build in flight against the OLD terrain: mark it — the
            // pump discards the landing payload and re-requests.
            it->second.stale = true;
            continue;
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
                              const Vec3& cameraPos, bool holdRequests) {
    frameIndices = 0; // the frame's draw*() calls sum into these
    frameHighInstances = 0;
    frameLowInstances = 0;
    frameUltraInstances = 0;
    pumpReseed(device); // async reseed landing point (main thread)
    if (barkGroupsDirty) {
        barkGroupsDirty = false;
        rebuildTreeBarkGroups(device);
    }
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
        if (it->second.stale) {
            // Scattered against terrain/water that changed mid-flight:
            // drop the payload, the ring re-requests with fresh params.
            streamer.chunks.erase(it);
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
            if (v >= kFirstPlant) {
                continue; // plants: no GI injection boxes (small cutouts)
            }
            // The compact CPU copy the GI injection boxes.
            const u8 kind = v < kFirstRock                          ? 0
                            : (v < kFirstBush || v >= kFirstDebris) ? 1
                                                                    : 2;
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
    if (!holdRequests) {
        streamer.requestMissing(
            camCx, camCz, viewRadius, kMaxRequestsPerFrame,
            [&](i32 cx, i32 cz, i32, i32) {
                return !streamer.chunks.contains(chunkKey(cx, cz));
            },
            [&](i32 cx, i32 cz, i32, i32) {
                streamer.chunks.emplace(chunkKey(cx, cz), Chunk {});
                const f32 fade = treeFadeEnd();
                streamer.enqueueBuild(cx, cz, [params, cx, cz, fade] {
                    return scatterProps(params, cx, cz, fade);
                });
            });
    }

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

} // namespace render
