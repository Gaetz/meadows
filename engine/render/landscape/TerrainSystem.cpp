#include "engine/render/landscape/TerrainSystem.hpp"

#include <algorithm>
#include <cmath>

#include "engine/assets/CookedTexture.hpp"
#include "engine/core/Clock.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "engine/render/MeshVertexLayout.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/render/landscape/SplatTextures.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {

constexpr const char* kTerrainShader = "terrain";
constexpr const char* kTerrainCasterShader = "shadow_terrain";

i32 camChunk(f32 worldCoord) {
    return chunkCoordOf(worldCoord, TerrainSystem::kChunkSize);
}

} // namespace

vector<MeshVertex> buildChunkVertices(const TerrainParams& params, i32 cx,
                                      i32 cz, u32 lod) {
    const u32 quads = TerrainSystem::lodQuads(lod);
    const u32 vertsPerSide = quads + 1;
    const f32 step = TerrainSystem::kChunkSize / static_cast<f32>(quads);
    const f32 originX = static_cast<f32>(cx) * TerrainSystem::kChunkSize;
    const f32 originZ = static_cast<f32>(cz) * TerrainSystem::kChunkSize;
    // Deep enough to cover the height error between this LOD's samples and
    // any neighbor's (error grows with the sample step).
    const f32 skirtDepth = 3.0f * step;

    vector<MeshVertex> vertices;
    vertices.reserve(vertsPerSide * vertsPerSide + 4 * vertsPerSide);
    for (u32 gz = 0; gz < vertsPerSide; ++gz) {
        for (u32 gx = 0; gx < vertsPerSide; ++gx) {
            const f32 x = originX + static_cast<f32>(gx) * step;
            const f32 z = originZ + static_cast<f32>(gz) * step;
            const f32 y = terrain::meshHeight(params, x, z, step);
            const Vec3 n = terrain::normal(params, x, z);
            vertices.push_back({
                .position = { x, y, z },
                .normal = n,
                .uv = { x / TerrainSystem::kChunkSize,
                        z / TerrainSystem::kChunkSize },
                // terrain.frag ignores a tint here (albedo comes from
                // the splats); .r carries the baked rock-exposure mask
                // instead — the cliff-material weight.
                .color = { terrain::rockExposureAt(params, x, z), 0.0f,
                           0.0f },
            });
        }
    }

    // Skirt ring: copies of the edge vertices pushed straight down. Same
    // normal/color as the edge, so lit skirts blend into the terrain when
    // they peek through an LOD seam. Order: north, south, west, east.
    const auto gridAt = [&](u32 gx, u32 gz) -> const MeshVertex& {
        return vertices[gz * vertsPerSide + gx];
    };
    const auto pushSkirt = [&](const MeshVertex& edge) {
        MeshVertex v = edge;
        v.position.y -= skirtDepth;
        vertices.push_back(v);
    };
    for (u32 i = 0; i < vertsPerSide; ++i) { pushSkirt(gridAt(i, 0)); }
    for (u32 i = 0; i < vertsPerSide; ++i) { pushSkirt(gridAt(i, quads)); }
    for (u32 j = 0; j < vertsPerSide; ++j) { pushSkirt(gridAt(0, j)); }
    for (u32 j = 0; j < vertsPerSide; ++j) { pushSkirt(gridAt(quads, j)); }
    return vertices;
}

vector<u32> buildChunkIndices(u32 lod) {
    const u32 quads = TerrainSystem::lodQuads(lod);
    const u32 vertsPerSide = quads + 1;
    vector<u32> indices;
    indices.reserve(quads * quads * 6 + 4 * quads * 12);
    for (u32 gz = 0; gz < quads; ++gz) {
        for (u32 gx = 0; gx < quads; ++gx) {
            const u32 i00 = gz * vertsPerSide + gx;
            const u32 i10 = i00 + 1;
            const u32 i01 = i00 + vertsPerSide;
            const u32 i11 = i01 + 1;
            // CCW seen from above (+Y), so CullMode::Back keeps the top.
            indices.insert(indices.end(), { i00, i01, i11, i00, i11, i10 });
        }
    }

    // Skirt quads, double-sided (both windings): per-edge winding bookkeeping
    // buys nothing at ~1k extra hidden triangles worst case.
    const u32 skirtBase = vertsPerSide * vertsPerSide;
    const auto quadDoubleSided = [&](u32 a, u32 b, u32 c, u32 d) {
        indices.insert(indices.end(),
                       { a, b, c, a, c, d, a, c, b, a, d, c });
    };
    const u32 north = skirtBase;
    const u32 south = skirtBase + vertsPerSide;
    const u32 west = skirtBase + 2 * vertsPerSide;
    const u32 east = skirtBase + 3 * vertsPerSide;
    for (u32 i = 0; i < quads; ++i) {
        quadDoubleSided(i, i + 1, north + i + 1, north + i); // gz = 0 edge
        const u32 s0 = quads * vertsPerSide + i;             // gz = quads edge
        quadDoubleSided(s0, s0 + 1, south + i + 1, south + i);
        const u32 w0 = i * vertsPerSide;                     // gx = 0 edge
        quadDoubleSided(w0, w0 + vertsPerSide, west + i + 1, west + i);
        const u32 e0 = i * vertsPerSide + quads;             // gx = quads edge
        quadDoubleSided(e0, e0 + vertsPerSide, east + i + 1, east + i);
    }
    return indices;
}

void TerrainSystem::create(rhi::Device& device, ShaderLibrary& shaders,
                           core::JobSystem& jobSystem,
                           const CookedSplatPaths& cooked) {
    streamer.create(jobSystem);

    for (u32 lod = 0; lod < kLodCount; ++lod) {
        const vector<u32> indices = buildChunkIndices(lod);
        indexCounts[lod] = static_cast<u32>(indices.size());
        // buildChunkIndices appends the skirt AFTER the grid, so the grid
        // prefix is a valid draw range on its own.
        gridIndexCounts[lod] = lodQuads(lod) * lodQuads(lod) * 6;
        indexBuffers[lod] =
            { device, device.createBuffer({ .usage = rhi::BufferUsage::Index,
                                  .size = indices.size() * sizeof(u32) },
                                indices.data()) };
    }

    // Vertex pools. Slot size = the LOD's deterministic vertex count
    // (grid + skirt). Capacity: LOD 0-3 rings are view-radius-independent
    // (lodForDistance bands), LOD4 fills the rest of the ring up to
    // kMaxViewRadius (+2 eviction hysteresis); the +64 headroom absorbs
    // LOD-swap transients (old slot lives until the new mesh lands).
    constexpr u32 kMaxRadius = static_cast<u32>(kMaxViewRadius) + 2;
    constexpr u32 kLod3Ring =
        kLod4CoreSide * kLod4CoreSide - kLod3CoreSide * kLod3CoreSide;
    constexpr u32 kLod4Ring =
        (2 * kMaxRadius + 1) * (2 * kMaxRadius + 1) -
        kLod4CoreSide * kLod4CoreSide;
    // LOD 0-2 hold several times their steady ring: fast flight leaves a
    // trail of chunks awaiting their LOD swap, each holding its old
    // slot. stealFurthestSlot() covers what the headroom cannot
    // (teleports). ~45 MB total at kMaxViewRadius 45.
    constexpr array<u32, kLodCount> kPoolCapacity {
        64, 128, 384, kLod3Ring + 64, kLod4Ring + 64
    };
    for (u32 lod = 0; lod < kLodCount; ++lod) {
        const u32 side = lodQuads(lod) + 1;
        VertexPool& pool = pools[lod];
        pool.slotVerts = side * side + 4 * side;
        pool.capacity = kPoolCapacity[lod];
        pool.buffer = { device, device.createBuffer(
            { .usage = rhi::BufferUsage::Vertex,
              .size = u64(pool.capacity) * pool.slotVerts *
                      sizeof(MeshVertex) },
            nullptr) };
        pool.freeSlots.clear();
        pool.freeSlots.reserve(pool.capacity);
        for (u32 s = pool.capacity; s > 0; --s) {
            pool.freeSlots.push_back(s - 1); // LIFO: slot 0 first
        }
    }

    if (device.caps().textureArrays) {
        cookedPaths = cooked;
        cookedPossible =
            cooked.complete() && device.caps().textureCompressionBC;
        splatSampler = { device, device.createSampler(
            { .mipmapFilter = true,
              .addressU = rhi::AddressMode::Repeat,
              .addressV = rhi::AddressMode::Repeat,
              .maxAnisotropy = 8.0f }) };
        buildMaterialArrays(device, cookedPossible);
    } else {
        LOG_WARN("TerrainSystem: no texture arrays on this backend — "
                 "terrain splatting disabled");
    }

    shaders.load(kTerrainShader, { { "FrameUbo", 0 } },
                 { { "uSplat", 0 },
                   { "uShadowMap", 1 },
                   { "uSplatHeight", 3 },
                   { "uTerrainShade0", 4 },
                   { "uTerrainShade1", 5 },
                   { "uSplatNormal", 8 } });
    buildPipeline(device, shaders);
    shaders.load(kTerrainCasterShader, { { "ShadowUbo", 1 } });
    buildCasterPipeline(device, shaders);
}

void TerrainSystem::buildMaterialArrays(rhi::Device& device, bool useCooked) {
    splatBindGroup.reset();
    splatTexture.reset();
    materialNormal.reset();
    materialOrm.reset();
    materialHeight.reset();

    // Cooked material arrays (Vulkan-only); the procedural tiles are the
    // fallback whenever anything is missing or fails.
    cookedMaterials = false;
    if (useCooked && cookedPossible) {
        const auto loadArray =
            [&](const str& path,
                rhi::TextureFormat expected) -> rhi::TextureHandle {
            auto tex = assets::loadCookedTexture(path);
            if (!tex || tex->format != expected ||
                tex->arrayLayers != kSplatArrayLayers) {
                if (tex) {
                    LOG_WARN("TerrainSystem: '{}' has {} layers "
                             "(splat needs {}) or wrong format — "
                             "procedural fallback",
                             path, tex->arrayLayers,
                             static_cast<u32>(kSplatArrayLayers));
                }
                return {};
            }
            return device.createTexture(tex->desc(), tex->payload.data());
        };
        rhi::TextureHandle albedo {};
        {
            auto tex = assets::loadCookedTexture(cookedPaths.albedo);
            if (tex && tex->format == rhi::TextureFormat::BC7_SRGB &&
                tex->arrayLayers == kSplatArrayLayers) {
                albedo = device.createTexture(tex->desc(),
                                              tex->payload.data());
                // The per-layer averages feed the blade root albedo
                // (grassAlbedoBase) — the CPU cannot decode BC7.
                const auto unpack = [](u32 packed) {
                    return Vec3 {
                        static_cast<f32>(packed & 0xffu) / 255.0f,
                        static_cast<f32>((packed >> 8) & 0xffu) / 255.0f,
                        static_cast<f32>((packed >> 16) & 0xffu) / 255.0f
                    };
                };
                for (u32 v = 0; v < kGrassVariantCount; ++v) {
                    grassBases[v] = unpack(
                        tex->layerAverages[grassVariantLayer(v)]);
                }
                // Semantic means: the far-mesh paint (layerAlbedoBase).
                for (u32 layer = 0; layer < SplatLayer_Count; ++layer) {
                    layerBases[layer] =
                        unpack(tex->layerAverages[layer]);
                }
            }
        }
        const rhi::TextureHandle normal =
            loadArray(cookedPaths.normal, rhi::TextureFormat::BC5_UNORM);
        const rhi::TextureHandle orm =
            loadArray(cookedPaths.orm, rhi::TextureFormat::BC7_UNORM);
        const rhi::TextureHandle height =
            loadArray(cookedPaths.height, rhi::TextureFormat::R16_UNORM);
        if (albedo.id != 0 && normal.id != 0 && orm.id != 0 &&
            height.id != 0) {
            splatTexture = { device, albedo };
            materialNormal = { device, normal };
            materialOrm = { device, orm };
            materialHeight = { device, height };
            cookedMaterials = true;
            LOG_INFO("TerrainSystem: cooked material arrays loaded "
                     "({} layers, BC7/BC5/R16)",
                     static_cast<u32>(kSplatArrayLayers));
        } else {
            // Free any array that did land before falling back.
            for (const rhi::TextureHandle h :
                 { albedo, normal, orm, height }) {
                if (h.id != 0) {
                    device.destroyTexture(h);
                }
            }
        }
    }
    if (!cookedMaterials) {
        // The procedural tile synthesis is gone (dev decision
        // 2026-08-06): the cooked .mtex library is THE material set.
        // Flat per-layer placeholders keep the game bootable when it is
        // absent; grassBases/layerBases keep their header defaults.
        LOG_WARN("TerrainSystem: cooked splat arrays missing — flat "
                 "placeholder materials");
        constexpr u32 kSize = 4;
        constexpr size_t kLayerBytes =
            static_cast<size_t>(kSize) * kSize * 4;
        const auto familyColor = [&](u32 layer) -> Vec3 {
            if (layer < SplatLayer_Count) {
                return layerBases[layer];
            }
            if (layer < 8) {
                return layerBases[SplatLayer_Grass];
            }
            if (layer < 11) {
                return layerBases[SplatLayer_Rock];
            }
            if (layer < 13) {
                return layerBases[SplatLayer_Snow];
            }
            return layerBases[SplatLayer_Sand];
        };
        vector<u8> albedoPixels(kLayerBytes * kSplatArrayLayers);
        vector<u8> normalPixels(kLayerBytes * kSplatArrayLayers);
        vector<f32> heightPixels(static_cast<size_t>(kSize) * kSize *
                                     kSplatArrayLayers,
                                 0.5f);
        for (u32 layer = 0; layer < kSplatArrayLayers; ++layer) {
            const Vec3 color = familyColor(layer);
            for (size_t at = layer * kLayerBytes;
                 at < (layer + 1) * kLayerBytes; at += 4) {
                albedoPixels[at + 0] = static_cast<u8>(
                    glm::clamp(color.r, 0.0f, 1.0f) * 255.0f);
                albedoPixels[at + 1] = static_cast<u8>(
                    glm::clamp(color.g, 0.0f, 1.0f) * 255.0f);
                albedoPixels[at + 2] = static_cast<u8>(
                    glm::clamp(color.b, 0.0f, 1.0f) * 255.0f);
                albedoPixels[at + 3] = 255;
                normalPixels[at + 0] = 128;
                normalPixels[at + 1] = 128;
                normalPixels[at + 2] = 255;
                normalPixels[at + 3] = 255;
            }
        }
        splatTexture = { device, device.createTexture(
            { .width = kSize,
              .height = kSize,
              .arrayLayers = kSplatArrayLayers,
              .format = rhi::TextureFormat::SRGBA8,
              .filter = rhi::FilterMode::Linear,
              .wrap = rhi::AddressMode::Repeat,
              .usage = rhi::TextureUsage_Sampled },
            albedoPixels.data()) };
        materialHeight = { device, device.createTexture(
            { .width = kSize,
              .height = kSize,
              .arrayLayers = kSplatArrayLayers,
              .format = rhi::TextureFormat::R16F,
              .filter = rhi::FilterMode::Linear,
              .wrap = rhi::AddressMode::Repeat,
              .usage = rhi::TextureUsage_Sampled },
            heightPixels.data()) };
        materialNormal = { device, device.createTexture(
            { .width = kSize,
              .height = kSize,
              .arrayLayers = kSplatArrayLayers,
              .format = rhi::TextureFormat::RGBA8,
              .filter = rhi::FilterMode::Linear,
              .wrap = rhi::AddressMode::Repeat,
              .usage = rhi::TextureUsage_Sampled },
            normalPixels.data()) };
    }
    splatBindGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = splatTexture,
                         .sampler = splatSampler },
                       { .binding = 3,
                         .texture = materialHeight,
                         .sampler = splatSampler },
                       { .binding = 8,
                         .texture = materialNormal,
                         .sampler = splatSampler } } }) };
}

void TerrainSystem::destroy(rhi::Device& device) {
    // Orphaned worker jobs keep pushing into the streamer's queue
    // harmlessly; results die with the last reference (TextureCache
    // teardown pattern) and stale generations drop on arrival.
    (void)device; // Unique handles free through their device
    streamer.invalidateAll([](Chunk&) {});
    resident = 0;
    pending = 0;
    pipeline.reset();
    casterPipeline.reset();
    for (u32 lod = 0; lod < kLodCount; ++lod) {
        indexBuffers[lod].reset();
        pools[lod] = {};
    }
    splatBindGroup.reset();
    splatSampler.reset();
    splatTexture.reset();
    materialNormal.reset();
    materialOrm.reset();
    materialHeight.reset();
    cookedMaterials = false;
}

void TerrainSystem::regenerate(rhi::Device& device) {
    (void)device;
    streamer.invalidateAll([&](Chunk& chunk) {
        if (chunk.residentLod != kNoLod) {
            freeSlot(chunk.residentLod, chunk.poolSlot);
        }
    });
    resident = 0;
    pending = 0;
    // update() re-requests the ring with the new params next frame.
}

void TerrainSystem::remeshChunks(const vector<u64>& keys) {
    for (const u64 key : keys) {
        const auto it = streamer.chunks.find(key);
        if (it == streamer.chunks.end()) {
            continue;
        }
        if (it->second.queuedLod != kNoLod) {
            // A build captured the OLD params before this invalidation:
            // its mesh would land stale and stick until an LOD change.
            // Mark it — pumpUploads re-enqueues on landing.
            it->second.remeshOnLand = true;
            continue;
        }
        if (it->second.residentLod == kNoLod) {
            continue; // never built: its next stream captures the new params
        }
        // Rebuild at the current LOD; enqueueBuild captures today's params
        // (the new patches). The old mesh keeps drawing until pumpUploads
        // swaps in the result (residentLod != kNoLod → the LOD-swap path).
        it->second.queuedLod = it->second.residentLod;
        ++pending;
        enqueueBuild(chunkKeyCx(key), chunkKeyCz(key),
                     it->second.residentLod);
    }
}

void TerrainSystem::update(rhi::Device& device, const Vec3& cameraPos,
                           bool holdRequests, bool boost) {
    frameIndices = 0; // the frame's draw*() calls sum into it
    for (VertexPool& pool : pools) {
        // Slots freed two frames ago finished cooling (their referencing
        // commands were consumed) — reusable from this frame on.
        pool.freeSlots.insert(pool.freeSlots.end(), pool.cooling[1].begin(),
                              pool.cooling[1].end());
        pool.cooling[1] = std::move(pool.cooling[0]);
        pool.cooling[0].clear();
    }
    pumpUploads(device, cameraPos, boost);
    if (!holdRequests) {
        requestMissing(cameraPos, boost);
    }
    evictFar(device, cameraPos);
}

void TerrainSystem::pumpUploads(rhi::Device& device, const Vec3& cameraPos,
                                bool boost) {
    // Time-budgeted on top of the count cap: 8 LOD0 uploads cost far more
    // than 8 LOD3 ones (the frame probe showed the count cap alone
    // spiking past 30 ms in Debug). At least one upload always lands, so
    // progress is guaranteed. (Budget loop in ChunkStreamer; this
    // lambda is the terrain-specific accept — the LOD-swap upload.)
    // Boosted while a loading veil hides the frame: stutter is invisible
    // there and the gate holds on `pending` draining.
    lastUploads = streamer.pump(
        boost ? kMaxUploadsPerFrame * 8 : kMaxUploadsPerFrame,
        boost ? kUploadMsBudget * 6.0 : kUploadMsBudget,
        [&](u64 key, auto& built) {
            const auto it = streamer.chunks.find(key);
            if (it == streamer.chunks.end() ||
                it->second.queuedLod != built.payload.lod) {
                // Evicted while in flight, or superseded by another LOD.
                return false;
            }
            Chunk& chunk = it->second;
            const u8 lod = built.payload.lod;
            const u32 slot = allocSlot(lod);
            if (slot == kNoSlot) {
                // Pool full: fast flight outran the LOD-swap pipeline and
                // stale far chunks hold the slots. Free the furthest
                // one's (it re-streams later at its true LOD; the far
                // mesh covers it) and drop THIS upload — its re-request
                // succeeds once the stolen slot leaves cooling.
                if (!stealFurthestSlot(lod, cameraPos)) {
                    LOG_WARN("TerrainSystem: LOD {} vertex pool full — "
                             "upload dropped",
                             lod);
                }
                chunk.queuedLod = kNoLod;
                chunk.remeshOnLand = false; // the re-request is fresh anyway
                --pending;
                return false;
            }
            if (chunk.residentLod == kNoLod) {
                ++resident;
            } else {
                // LOD swap: the old mesh drew until this very frame — its
                // slot frees only now, so there is no hole.
                freeSlot(chunk.residentLod, chunk.poolSlot);
            }
            device.updateBuffer(pools[lod].buffer,
                                built.payload.vertices.data(),
                                built.payload.vertices.size() *
                                    sizeof(MeshVertex),
                                u64(slot) * pools[lod].slotVerts *
                                    sizeof(MeshVertex));
            chunk.poolSlot = slot;
            chunk.residentLod = lod;
            chunk.queuedLod = kNoLod;
            chunk.minY = built.payload.minY;
            chunk.maxY = built.payload.maxY;
            --pending;
            if (chunk.remeshOnLand) {
                // This mesh was built against pre-invalidation params
                // (remeshChunks caught it in flight): it draws now — no
                // hole — while a rebuild with today's params is queued.
                chunk.remeshOnLand = false;
                chunk.queuedLod = lod;
                ++pending;
                enqueueBuild(chunkKeyCx(key), chunkKeyCz(key), lod);
            }
            return true;
        });
}

void TerrainSystem::requestMissing(const Vec3& cameraPos, bool boost) {
    const i32 camCx = camChunk(cameraPos.x);
    const i32 camCz = camChunk(cameraPos.z);
    // Center-out: the terrain under the camera arrives first, holes stay at
    // the horizon. Anything past the request budget is simply re-detected
    // next frame — the state IS the queue. The desired LOD is re-derived
    // from the ring distance in both lambdas (cheap, stateless).
    const auto lodAt = [](i32 dx, i32 dz) {
        return static_cast<u8>(
            lodForDistance(std::max(std::abs(dx), std::abs(dz))));
    };
    streamer.requestMissing(
        camCx, camCz, viewRadius,
        // Bigger rings fill faster (cold start / teleport at radius 45
        // is ~8k chunks); the workers absorb it, the frame thread only
        // pays the enqueue. Behind a loading veil the burst-flattening
        // cap serves nothing — open it wide so the gate drains fast.
        boost ? kMaxRequestsPerFrame * 16
              : (viewRadius > 24 ? kMaxRequestsPerFrame * 2
                                 : kMaxRequestsPerFrame),
        [&](i32 cx, i32 cz, i32 dx, i32 dz) {
            const auto it = streamer.chunks.find(chunkKey(cx, cz));
            if (it == streamer.chunks.end()) {
                return true; // missing entirely
            }
            // LOD change: request the new mesh once the previous request
            // (if any) has landed; the resident mesh keeps drawing.
            const Chunk& chunk = it->second;
            return chunk.queuedLod == kNoLod &&
                   chunk.residentLod != lodAt(dx, dz);
        },
        [&](i32 cx, i32 cz, i32 dx, i32 dz) {
            const u8 lod = lodAt(dx, dz);
            const auto it = streamer.chunks.find(chunkKey(cx, cz));
            if (it == streamer.chunks.end()) {
                Chunk chunk;
                chunk.queuedLod = lod;
                streamer.chunks.emplace(chunkKey(cx, cz), std::move(chunk));
            } else {
                it->second.queuedLod = lod;
            }
            ++pending;
            enqueueBuild(cx, cz, lod);
        });
}

void TerrainSystem::enqueueBuild(i32 cx, i32 cz, u8 lod) {
    streamer.enqueueBuild(cx, cz, [chunkParams = params, cx, cz, lod] {
        BuiltMesh built { lod, buildChunkVertices(chunkParams, cx, cz, lod) };
        // Height range for the frustum AABB (skirts included: they
        // hang below, keeping the bound conservative).
        built.minY = built.vertices[0].position.y;
        built.maxY = built.minY;
        for (const MeshVertex& vertex : built.vertices) {
            built.minY = glm::min(built.minY, vertex.position.y);
            built.maxY = glm::max(built.maxY, vertex.position.y);
        }
        return built;
    });
}

void TerrainSystem::evictFar(rhi::Device& /*device*/, const Vec3& cameraPos) {
    streamer.evictFar(camChunk(cameraPos.x), camChunk(cameraPos.z),
                      viewRadius + 2, [&](Chunk& chunk) {
                          if (chunk.residentLod != kNoLod) {
                              freeSlot(chunk.residentLod, chunk.poolSlot);
                              --resident;
                          }
                          if (chunk.queuedLod != kNoLod) {
                              // In-flight result dropped on arrival.
                              --pending;
                          }
                      });
}

bool TerrainSystem::stealFurthestSlot(u32 lod, const Vec3& cameraPos) {
    const i32 camCx = camChunk(cameraPos.x);
    const i32 camCz = camChunk(cameraPos.z);
    Chunk* victim = nullptr;
    i32 victimCheb = -1;
    for (auto& [key, chunk] : streamer.chunks) {
        if (chunk.residentLod != lod) {
            continue;
        }
        const i32 cheb =
            std::max(std::abs(chunkKeyCx(key) - camCx),
                     std::abs(chunkKeyCz(key) - camCz));
        if (cheb > victimCheb) {
            victimCheb = cheb;
            victim = &chunk;
        }
    }
    if (victim == nullptr) {
        return false;
    }
    freeSlot(lod, victim->poolSlot);
    victim->poolSlot = kNoSlot;
    victim->residentLod = kNoLod; // stops drawing; re-streams at true LOD
    --resident;
    return true;
}

u32 TerrainSystem::allocSlot(u32 lod) {
    VertexPool& pool = pools[lod];
    if (pool.freeSlots.empty()) {
        return kNoSlot;
    }
    const u32 slot = pool.freeSlots.back();
    pool.freeSlots.pop_back();
    return slot;
}

void TerrainSystem::freeSlot(u32 lod, u32 slot) {
    if (slot != kNoSlot && lod < kLodCount) {
        pools[lod].cooling[0].push_back(slot);
    }
}

void TerrainSystem::buildPipeline(rhi::Device& device, ShaderLibrary& shaders) {
    // U3-7: the assignment frees the previous pipeline.
    pipeline = { device, device.createPipeline(
        { .shader = shaders.get(kTerrainShader),
          .vertexBuffers = { meshVertexLayout() }, // U3-5
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Greater }, // reversed-Z
          .cull = rhi::CullMode::Back,
          .wireframe = wireframe }) };
    shaderGeneration = shaders.generation(kTerrainShader);
}

void TerrainSystem::buildCasterPipeline(rhi::Device& device,
                                        ShaderLibrary& shaders) {
    casterPipeline = { device, device.createPipeline(
        { .shader = shaders.get(kTerrainCasterShader),
          .vertexBuffers = { meshVertexPositionLayout() }, // U3-5
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Less },
          .cull = rhi::CullMode::Back,
          // Polygon offset: the first line of defense against shadow acne.
          .depthBias = 4.0f,
          .depthBiasSlope = 2.5f }) };
    casterShaderGeneration = shaders.generation(kTerrainCasterShader);
}

void TerrainSystem::refreshPipeline(rhi::Device& device,
                                    ShaderLibrary& shaders) {
    if (shaders.generation(kTerrainShader) != shaderGeneration) {
        buildPipeline(device, shaders);
    }
    if (shaders.generation(kTerrainCasterShader) != casterShaderGeneration) {
        buildCasterPipeline(device, shaders);
    }
}

void TerrainSystem::setWireframe(bool enabled, rhi::Device& device,
                                 ShaderLibrary& shaders) {
    if (wireframe != enabled) {
        wireframe = enabled;
        buildPipeline(device, shaders);
    }
}

void TerrainSystem::draw(rhi::CommandBuffer& cmd,
                         rhi::BindGroupHandle frameBindGroup,
                         rhi::BindGroupHandle shadowBindGroup,
                         const Frustum* frustum,
                         const std::unordered_set<u64>* occluded) {
    cmd.setPipeline(pipeline);
    cmd.setBindGroup(0, frameBindGroup);
    if (splatBindGroup.id() != 0) {
        cmd.setBindGroup(1, splatBindGroup);
    }
    if (shadowBindGroup.id != 0) {
        cmd.setBindGroup(2, shadowBindGroup);
    }
    u32 drawn = 0;
    // Grouped by LOD so the shared index buffer binds once per level.
    for (u32 lod = 0; lod < kLodCount; ++lod) {
        bool indexBufferBound = false;
        for (const auto& [key, chunk] : streamer.chunks) {
            if (chunk.residentLod != lod) {
                continue;
            }
            if (occluded && occluded->contains(key)) {
                continue; // hidden behind a ridge (GpuOcclusion)
            }
            if (frustum) {
                const f32 x0 =
                    static_cast<f32>(chunkKeyCx(key)) * kChunkSize;
                const f32 z0 =
                    static_cast<f32>(chunkKeyCz(key)) * kChunkSize;
                if (!frustum->intersectsAabb(
                        { x0, chunk.minY, z0 },
                        { x0 + kChunkSize, chunk.maxY, z0 + kChunkSize })) {
                    continue;
                }
            }
            if (!indexBufferBound) {
                cmd.setIndexBuffer(indexBuffers[lod], rhi::IndexFormat::U32);
                indexBufferBound = true;
            }
            cmd.setVertexBuffer(0, pools[lod].buffer,
                                u64(chunk.poolSlot) * pools[lod].slotVerts *
                                    sizeof(MeshVertex));
            cmd.drawIndexed(indexCounts[lod]);
            frameIndices += indexCounts[lod];
            ++drawn;
        }
    }
    if (frustum) {
        lastDrawn = drawn;
    }
}

void TerrainSystem::drawIndirect(rhi::CommandBuffer& cmd,
                                 rhi::BindGroupHandle frameBindGroup,
                                 rhi::BindGroupHandle shadowBindGroup,
                                 rhi::BufferHandle commands,
                                 const u32* lodFirst, const u32* lodCount) {
    cmd.setPipeline(pipeline);
    cmd.setBindGroup(0, frameBindGroup);
    if (splatBindGroup.id() != 0) {
        cmd.setBindGroup(1, splatBindGroup);
    }
    if (shadowBindGroup.id != 0) {
        cmd.setBindGroup(2, shadowBindGroup);
    }
    constexpr u32 kStride = sizeof(rhi::DrawIndexedIndirectCommand);
    for (u32 lod = 0; lod < kLodCount; ++lod) {
        if (lodCount[lod] == 0) {
            continue;
        }
        cmd.setIndexBuffer(indexBuffers[lod], rhi::IndexFormat::U32);
        cmd.setVertexBuffer(0, pools[lod].buffer);
        cmd.drawIndexedIndirect(commands, u64(lodFirst[lod]) * kStride,
                                lodCount[lod], kStride);
        // Upper bound (culled commands cost nothing on the GPU but the
        // counter cannot know) — the panel reads it as "candidates".
        frameIndices += indexCounts[lod] * lodCount[lod];
    }
    lastDrawn = lodCount[0] + lodCount[1] + lodCount[2] + lodCount[3];
}

// Skirts are excluded here (gridIndexCounts): seen from the sun they are
// vertical walls along chunk borders and would print shadow lines.
void TerrainSystem::drawDepth(rhi::CommandBuffer& cmd,
                              rhi::BindGroupHandle casterBindGroup,
                              const Vec3& cameraPos, i32 maxChunkDistance,
                              const Frustum* frustum) {
    const i32 camCx = camChunk(cameraPos.x);
    const i32 camCz = camChunk(cameraPos.z);
    cmd.setPipeline(casterPipeline);
    cmd.setBindGroup(0, casterBindGroup);
    for (u32 lod = 0; lod < kLodCount; ++lod) {
        bool indexBufferBound = false;
        for (const auto& [key, chunk] : streamer.chunks) {
            if (chunk.residentLod != lod) {
                continue;
            }
            const i32 cx = chunkKeyCx(key);
            const i32 cz = chunkKeyCz(key);
            if (std::max(std::abs(cx - camCx), std::abs(cz - camCz)) >
                maxChunkDistance) {
                continue; // beyond the last cascade
            }
            if (frustum != nullptr) {
                const f32 x0 = static_cast<f32>(cx) * kChunkSize;
                const f32 z0 = static_cast<f32>(cz) * kChunkSize;
                if (!frustum->intersectsAabb(
                        { x0, chunk.minY, z0 },
                        { x0 + kChunkSize, chunk.maxY, z0 + kChunkSize })) {
                    continue; // outside this cascade's ortho volume
                }
            }
            if (!indexBufferBound) {
                cmd.setIndexBuffer(indexBuffers[lod], rhi::IndexFormat::U32);
                indexBufferBound = true;
            }
            cmd.setVertexBuffer(0, pools[lod].buffer,
                                u64(chunk.poolSlot) * pools[lod].slotVerts *
                                    sizeof(MeshVertex));
            cmd.drawIndexed(gridIndexCounts[lod]);
            frameIndices += gridIndexCounts[lod];
        }
    }
}

} // namespace render
