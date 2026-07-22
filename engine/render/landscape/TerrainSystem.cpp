#include "engine/render/landscape/TerrainSystem.hpp"

#include <algorithm>
#include <cmath>

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

// Per-vertex material color — the base tint the splat tiles blend over:
// sand at the shoreline, grass on plains, rock on
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
                           core::JobSystem& jobSystem) {
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

    if (device.caps().textureArrays) {
        const vector<u8> splatPixels = buildSplatTilePixels();
        splatTexture = { device, device.createTexture(
            { .width = kSplatTileSize,
              .height = kSplatTileSize,
              .arrayLayers = SplatLayer_Count,
              .mipLevels = 9, // full chain for a 256 tile
              // sRGB: tiles are authored in display space, decoded to linear
              // on sample — the HDR pipeline lights in linear.
              .format = rhi::TextureFormat::SRGBA8,
              .filter = rhi::FilterMode::Linear,
              .wrap = rhi::AddressMode::Repeat,
              .usage = rhi::TextureUsage_Sampled },
            splatPixels.data()) };
        device.generateMipmaps(splatTexture);
        splatSampler = { device, device.createSampler(
            { .mipmapFilter = true,
              .addressU = rhi::AddressMode::Repeat,
              .addressV = rhi::AddressMode::Repeat,
              .maxAnisotropy = 8.0f }) };
        splatBindGroup = { device, device.createBindGroup(
            { .entries = { { .binding = 0,
                             .texture = splatTexture,
                             .sampler = splatSampler } } }) };
    } else {
        LOG_WARN("TerrainSystem: no texture arrays on this backend — "
                 "terrain splatting disabled");
    }

    shaders.load(kTerrainShader, { { "FrameUbo", 0 } },
                 { { "uSplat", 0 }, { "uShadowMap", 1 } });
    buildPipeline(device, shaders);
    shaders.load(kTerrainCasterShader, { { "ShadowUbo", 1 } });
    buildCasterPipeline(device, shaders);
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
    }
    splatBindGroup.reset();
    splatSampler.reset();
    splatTexture.reset();
}

void TerrainSystem::regenerate(rhi::Device& device) {
    (void)device; // Unique buffers free through their device
    streamer.invalidateAll([](Chunk&) {});
    resident = 0;
    pending = 0;
    // update() re-requests the ring with the new params next frame.
}

void TerrainSystem::remeshChunks(const vector<u64>& keys) {
    for (const u64 key : keys) {
        const auto it = streamer.chunks.find(key);
        if (it == streamer.chunks.end() ||
            it->second.residentLod == kNoLod ||
            it->second.queuedLod != kNoLod) {
            continue; // not drawn yet, or a build is already in flight
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

void TerrainSystem::update(rhi::Device& device, const Vec3& cameraPos) {
    frameIndices = 0; // the frame's draw*() calls sum into it
    pumpUploads(device);
    requestMissing(cameraPos);
    evictFar(device, cameraPos);
}

void TerrainSystem::pumpUploads(rhi::Device& device) {
    // Time-budgeted on top of the count cap: 8 LOD0 uploads cost far more
    // than 8 LOD3 ones (the frame probe showed the count cap alone
    // spiking past 30 ms in Debug). At least one upload always lands, so
    // progress is guaranteed. (Budget loop in ChunkStreamer; this
    // lambda is the terrain-specific accept — the LOD-swap upload.)
    lastUploads = streamer.pump(
        kMaxUploadsPerFrame, kUploadMsBudget, [&](u64 key, auto& built) {
            const auto it = streamer.chunks.find(key);
            if (it == streamer.chunks.end() ||
                it->second.queuedLod != built.payload.lod) {
                // Evicted while in flight, or superseded by another LOD.
                return false;
            }
            Chunk& chunk = it->second;
            if (chunk.residentLod == kNoLod) {
                ++resident;
            }
            // LOD swap: the old mesh drew until this very frame — no hole
            // (the assignment frees it through the Unique wrapper).
            chunk.vertexBuffer = { device, device.createBuffer(
                { .usage = rhi::BufferUsage::Vertex,
                  .size = built.payload.vertices.size() *
                          sizeof(MeshVertex) },
                built.payload.vertices.data()) };
            chunk.residentLod = built.payload.lod;
            chunk.queuedLod = kNoLod;
            chunk.minY = built.payload.minY;
            chunk.maxY = built.payload.maxY;
            --pending;
            return true;
        });
}

void TerrainSystem::requestMissing(const Vec3& cameraPos) {
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
        camCx, camCz, kViewRadius, kMaxRequestsPerFrame,
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

void TerrainSystem::evictFar(rhi::Device& device, const Vec3& cameraPos) {
    streamer.evictFar(camChunk(cameraPos.x), camChunk(cameraPos.z),
                      kEvictRadius, [&](Chunk& chunk) {
                          // U3-7: the erase frees the vertex buffer.
                          if (chunk.residentLod != kNoLod) {
                              --resident;
                          }
                          if (chunk.queuedLod != kNoLod) {
                              // In-flight result dropped on arrival.
                              --pending;
                          }
                      });
}

void TerrainSystem::buildPipeline(rhi::Device& device, ShaderLibrary& shaders) {
    // U3-7: the assignment frees the previous pipeline.
    pipeline = { device, device.createPipeline(
        { .shader = shaders.get(kTerrainShader),
          .vertexBuffers = { meshVertexLayout() }, // U3-5
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Less },
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
            cmd.setVertexBuffer(0, chunk.vertexBuffer);
            cmd.drawIndexed(indexCounts[lod]);
            frameIndices += indexCounts[lod];
            ++drawn;
        }
    }
    if (frustum) {
        lastDrawn = drawn;
    }
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
            cmd.setVertexBuffer(0, chunk.vertexBuffer);
            cmd.drawIndexed(gridIndexCounts[lod]);
            frameIndices += gridIndexCounts[lod];
        }
    }
}

} // namespace render
