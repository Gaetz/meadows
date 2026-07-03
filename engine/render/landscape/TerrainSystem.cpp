#include "engine/render/landscape/TerrainSystem.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/render/landscape/SplatTextures.hpp"
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
    jobs = &jobSystem;
    shared = std::make_shared<Shared>();

    for (u32 lod = 0; lod < kLodCount; ++lod) {
        const vector<u32> indices = buildChunkIndices(lod);
        indexCounts[lod] = static_cast<u32>(indices.size());
        indexBuffers[lod] =
            device.createBuffer({ .usage = rhi::BufferUsage::Index,
                                  .size = indices.size() * sizeof(u32) },
                                indices.data());
    }

    if (device.caps().textureArrays) {
        const vector<u8> splatPixels = buildSplatTilePixels();
        splatTexture = device.createTexture(
            { .width = kSplatTileSize,
              .height = kSplatTileSize,
              .arrayLayers = SplatLayer_Count,
              .mipLevels = 9, // full chain for a 256 tile
              .format = rhi::TextureFormat::RGBA8,
              .filter = rhi::FilterMode::Linear,
              .wrap = rhi::AddressMode::Repeat,
              .usage = rhi::TextureUsage_Sampled },
            splatPixels.data());
        device.generateMipmaps(splatTexture);
        splatSampler = device.createSampler(
            { .mipmapFilter = true,
              .addressU = rhi::AddressMode::Repeat,
              .addressV = rhi::AddressMode::Repeat,
              .maxAnisotropy = 8.0f });
        splatBindGroup = device.createBindGroup(
            { .entries = { { .binding = 0,
                             .texture = splatTexture,
                             .sampler = splatSampler } } });
    } else {
        LOG_WARN("TerrainSystem: no texture arrays on this backend — "
                 "terrain splatting disabled");
    }

    shaders.load(kTerrainShader, { { "FrameUbo", 0 } },
                 { { "uSplat", 0 } });
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
    pipeline = {};
    for (u32 lod = 0; lod < kLodCount; ++lod) {
        device.destroyBuffer(indexBuffers[lod]);
        indexBuffers[lod] = {};
    }
    device.destroyBindGroup(splatBindGroup);
    device.destroySampler(splatSampler);
    device.destroyTexture(splatTexture);
    splatBindGroup = {};
    splatSampler = {};
    splatTexture = {};
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
        if (it == chunks.end() || it->second.queuedLod != built.lod) {
            continue; // evicted while in flight, or superseded by another LOD
        }
        Chunk& chunk = it->second;
        if (chunk.residentLod == kNoLod) {
            ++resident;
        } else {
            // LOD swap: the old mesh drew until this very frame — no hole.
            device.destroyBuffer(chunk.vertexBuffer);
        }
        chunk.vertexBuffer = device.createBuffer(
            { .usage = rhi::BufferUsage::Vertex,
              .size = built.vertices.size() * sizeof(MeshVertex) },
            built.vertices.data());
        chunk.residentLod = built.lod;
        chunk.queuedLod = kNoLod;
        --pending;
        ++lastUploads;
    }
}

void TerrainSystem::requestMissing(const Vec3& cameraPos) {
    const i32 camCx = chunkCoordOf(cameraPos.x);
    const i32 camCz = chunkCoordOf(cameraPos.z);

    struct Candidate {
        i32 cx, cz, dist2;
        u8 lod;
    };
    vector<Candidate> wanted;
    for (i32 dz = -kViewRadius; dz <= kViewRadius; ++dz) {
        for (i32 dx = -kViewRadius; dx <= kViewRadius; ++dx) {
            const i32 cx = camCx + dx;
            const i32 cz = camCz + dz;
            const u8 lod = static_cast<u8>(
                lodForDistance(std::max(std::abs(dx), std::abs(dz))));
            const auto it = chunks.find(keyOf(cx, cz));
            if (it == chunks.end()) {
                wanted.push_back({ cx, cz, dx * dx + dz * dz, lod });
                continue;
            }
            // LOD change: request the new mesh once the previous request (if
            // any) has landed; the resident mesh keeps drawing meanwhile.
            Chunk& chunk = it->second;
            if (chunk.queuedLod == kNoLod && chunk.residentLod != lod) {
                chunk.queuedLod = lod;
                ++pending;
                enqueueBuild(cx, cz, lod);
            }
        }
    }
    // Center-out: the terrain under the camera arrives first, holes stay at
    // the horizon.
    std::sort(wanted.begin(), wanted.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.dist2 < b.dist2;
              });

    for (const Candidate& c : wanted) {
        Chunk chunk;
        chunk.queuedLod = c.lod;
        chunks.emplace(keyOf(c.cx, c.cz), chunk);
        ++pending;
        enqueueBuild(c.cx, c.cz, c.lod);
    }
}

void TerrainSystem::enqueueBuild(i32 cx, i32 cz, u8 lod) {
    jobs->enqueue(
        [sharedRef = shared, chunkParams = params, cx, cz, lod,
         gen = generation] {
            sharedRef->built.push(
                { cx, cz, lod, gen,
                  buildChunkVertices(chunkParams, cx, cz, lod) });
        });
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
        if (it->second.residentLod != kNoLod) {
            device.destroyBuffer(it->second.vertexBuffer);
            --resident;
        }
        if (it->second.queuedLod != kNoLod) {
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
          .cull = rhi::CullMode::Back,
          .wireframe = wireframe });
    shaderGeneration = shaders.generation(kTerrainShader);
}

void TerrainSystem::refreshPipeline(rhi::Device& device,
                                    ShaderLibrary& shaders) {
    if (shaders.generation(kTerrainShader) != shaderGeneration) {
        buildPipeline(device, shaders);
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
                         rhi::BindGroupHandle frameBindGroup) {
    cmd.setPipeline(pipeline);
    cmd.setBindGroup(0, frameBindGroup);
    if (splatBindGroup.id != 0) {
        cmd.setBindGroup(1, splatBindGroup);
    }
    // Grouped by LOD so the shared index buffer binds once per level.
    for (u32 lod = 0; lod < kLodCount; ++lod) {
        bool indexBufferBound = false;
        for (const auto& [key, chunk] : chunks) {
            if (chunk.residentLod != lod) {
                continue;
            }
            if (!indexBufferBound) {
                cmd.setIndexBuffer(indexBuffers[lod], rhi::IndexFormat::U32);
                indexBufferBound = true;
            }
            cmd.setVertexBuffer(0, chunk.vertexBuffer);
            cmd.drawIndexed(indexCounts[lod]);
        }
    }
}

} // namespace render
