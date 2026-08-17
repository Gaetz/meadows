// The render trade: pipelines, LOD selection (canopyLevel/levelMesh),
// the three draw paths (main, indirect, depth) and the GI prop
// collection. The scatter / assets / streaming trades live in their
// own TUs (VegetationScatter/Assets/Streaming.cpp).
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

// Per-variant bark material push (tree.frag BarkPush): the species'
// draw-time knobs — tint + tile density, hex lattice cell + sharpness.
namespace {
struct BarkPush {
    Vec4 tintTile; // rgb = tint, w = tiles per meter
    Vec4 hex;      // x = lattice cell (uv), y = seam sharpness
};
} // namespace



const ColonizedTreeParams&
VegetationSystem::barkParamsFor(u32 variant) const {
    if (variant < kTreeVariants && treeSpecies[variant]) {
        return treeSpecies[variant]->params;
    }
    if (variant >= kFirstBush && variant < kFirstBush + kBushVariants &&
        bushSpecies) {
        return bushSpecies->params;
    }
    return colonizedTreeParams;
}

void VegetationSystem::buildPipeline(rhi::Device& device,
                                     ShaderLibrary& shaders) {
    shaders.beginWatch();
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
          .cull = rhi::CullMode::Back,
          .pushConstantSize = sizeof(BarkPush) }) };
    shaderWatch = shaders.endWatch();
}

void VegetationSystem::buildCasterPipeline(rhi::Device& device,
                                           ShaderLibrary& shaders) {
    shaders.beginWatch();
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
    casterShaderWatch = shaders.endWatch();
}

void VegetationSystem::refreshPipeline(rhi::Device& device,
                                       ShaderLibrary& shaders) {
    if (shaderWatch.changed(shaders)) {
        buildPipeline(device, shaders);
    }
    if (casterShaderWatch.changed(shaders)) {
        buildCasterPipeline(device, shaders);
    }
}

// True when the camera stands within kNearDetailDistance of the
// chunk's XZ square — the hero-near upgrade zone. Shared by draw() and
// collectDrawCandidates so both paths pick identically.
static bool chunkNear(i32 cx, i32 cz, const Vec3& cameraPos) {
    const f32 size = TerrainSystem::kChunkSize;
    const f32 x0 = static_cast<f32>(cx) * size;
    const f32 z0 = static_cast<f32>(cz) * size;
    const f32 dx = glm::max(
        glm::max(x0 - cameraPos.x, cameraPos.x - (x0 + size)), 0.0f);
    const f32 dz = glm::max(
        glm::max(z0 - cameraPos.z, cameraPos.z - (z0 + size)), 0.0f);
    return dx * dx + dz * dz < VegetationSystem::kNearDetailDistance *
                                   VegetationSystem::kNearDetailDistance;
}

u32 VegetationSystem::canopyLevel(const VariantMesh& mesh, u32 variant,
                                  i32 cheb, bool nearChunk,
                                  bool forceLowDetail) const {
    if (mesh.lowIndexCount == 0) {
        return 0u; // rocks, bushes, authored overrides: main mesh only
    }
    if (forceLowDetail) {
        return mesh.ultraIndexCount != 0 ? 2u : 1u;
    }
    // Hero-near twin: exact world distance to the chunk square, so a
    // tree just across a chunk border still upgrades.
    if (mesh.nearIndexCount != 0 && nearChunk) {
        return 3u;
    }
    // Plants fade at ~60 m: the tree radii (2/4 chunks) would keep them
    // full-detail everywhere they are visible. Hero mesh in the camera
    // chunk ring only; twins carry the rest.
    const i32 highRadius = variant >= kFirstPlant ? 0 : highDetailRadius;
    if (cheb <= highRadius) {
        return 0u;
    }
    if (mesh.ultraIndexCount == 0 || cheb <= lowDetailRadius) {
        return 1u;
    }
    return 2u;
}

VegetationSystem::LevelMesh
VegetationSystem::levelMesh(const VariantMesh& mesh, u32 level) {
    switch (level) {
    case 3:
        return { mesh.nearVertexBuffer.get(), mesh.nearIndexBuffer.get(),
                 mesh.nearIndexCount };
    case 1:
        return { mesh.lowVertexBuffer.get(), mesh.lowIndexBuffer.get(),
                 mesh.lowIndexCount };
    case 2:
        return { mesh.ultraVertexBuffer.get(), mesh.ultraIndexBuffer.get(),
                 mesh.ultraIndexCount };
    default:
        return { mesh.vertexBuffer.get(), mesh.indexBuffer.get(),
                 mesh.indexCount };
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
    // ONE pass over the chunk map: visibility verdict + the LOD inputs
    // (cheb, hero-near) cached per chunk — the (variant × level) loops
    // below then walk this compact vector instead of re-traversing the
    // map per pair. Chunk pointers stay stable (nothing mutates the map
    // during draw).
    const i32 camCx = chunkCoordOf(cameraPos.x, TerrainSystem::kChunkSize);
    const i32 camCz = chunkCoordOf(cameraPos.z, TerrainSystem::kChunkSize);
    drawScratch.clear();
    for (const auto& [key, chunk] : streamer.chunks) {
        if (!chunk.resident || chunk.total == 0 ||
            (culling && !chunkVisible(key, chunk))) {
            continue;
        }
        const i32 cx = chunkKeyCx(key);
        const i32 cz = chunkKeyCz(key);
        const i32 cheb =
            std::max(std::abs(cx - camCx), std::abs(cz - camCz));
        drawScratch.push_back(
            { &chunk, cheb, cheb <= 1 && chunkNear(cx, cz, cameraPos) });
    }
    if (culling) {
        lastDrawn = static_cast<u32>(drawScratch.size());
    }

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
        // The slot's bark group, like the streamed path — without it
        // the builder's specimen showed flat-normal trunks while the
        // forest showed bark.
        if (mesh.albedoGroup.get().id != 0) {
            cmd.setBindGroup(1, mesh.albedoGroup.get());
        }
        const BarkPush push { Vec4(colonizedTreeParams.barkTint,
                                   colonizedTreeParams.barkTileScale),
                              Vec4(colonizedTreeParams.barkHexCell,
                                   colonizedTreeParams.barkHexSharpness,
                                   0.0f, 0.0f) };
        cmd.setPushConstants(&push, sizeof(push));
        // The specimen IS the close-up case: near twin when built.
        const bool near = mesh.nearIndexCount != 0;
        cmd.setVertexBuffer(0, near ? mesh.nearVertexBuffer.get()
                                    : mesh.vertexBuffer.get());
        cmd.setIndexBuffer(near ? mesh.nearIndexBuffer.get()
                                : mesh.indexBuffer.get(),
                           rhi::IndexFormat::U32);
        cmd.setVertexBuffer(1, showcaseInstances.get());
        const u32 count = near ? mesh.nearIndexCount : mesh.indexCount;
        cmd.drawIndexed(count, showcaseCount, 0, 0);
        frameIndices += count * showcaseCount;
        frameHighInstances += showcaseCount;
        return;
    }
    // Variant-major, split by LOD (canopyLevel — THREE levels: 320-face
    // lobes near, 80-face twins mid, 20-face ultra beyond
    // lowDetailRadius, and always in mirrored/downsampled passes; 3 =
    // the hero-near twin): bind each mesh level once, then one
    // instanced draw per chunk holding that variant (firstInstance =
    // offset into the chunk's variant-sorted buffer; needs baseInstance,
    // present on 4.6).
    bool albedoBound = false;
    for (u32 v = 0; v < variantLimit; ++v) {
        const VariantMesh& mesh = variantMeshes[v];
        const ColonizedTreeParams& bp = barkParamsFor(v);
        const BarkPush push { Vec4(bp.barkTint, bp.barkTileScale),
                              Vec4(bp.barkHexCell, bp.barkHexSharpness,
                                   0.0f, 0.0f) };
        cmd.setPushConstants(&push, sizeof(push));
        // Textured plants swap group 1 for the variant's albedo (same
        // layout as the leaf-mask atlas); restore the atlas after.
        if (mesh.albedoGroup.get().id != 0) {
            cmd.setBindGroup(1, mesh.albedoGroup.get());
            albedoBound = true;
        } else if (albedoBound) {
            cmd.setBindGroup(1, leafMaskGroup);
            albedoBound = false;
        }
        const u32 levels = mesh.lowIndexCount == 0
                               ? 1u
                               : mesh.ultraIndexCount == 0 ? 2u
                               : mesh.nearIndexCount == 0  ? 3u
                                                           : 4u;
        for (u32 level = 0; level < levels; ++level) {
            const LevelMesh lod = levelMesh(mesh, level);
            bool meshBound = false;
            for (const DrawChunkRef& ref : drawScratch) {
                const Chunk& chunk = *ref.chunk;
                if (chunk.counts[v] == 0 ||
                    canopyLevel(mesh, v, ref.cheb, ref.nearChunk,
                                forceLowDetail) != level) {
                    continue;
                }
                if (!meshBound) {
                    cmd.setVertexBuffer(0, lod.vertices);
                    cmd.setIndexBuffer(lod.indices, rhi::IndexFormat::U32);
                    meshBound = true;
                }
                cmd.setVertexBuffer(1, instancePool.buffer.get(),
                                    u64(chunk.poolOffset) *
                                        sizeof(Instance));
                cmd.drawIndexed(lod.indexCount, chunk.counts[v], 0,
                                chunk.firstInstance[v]);
                frameIndices += lod.indexCount * chunk.counts[v];
                (level == 0 || level == 3 ? frameHighInstances
                 : level == 1             ? frameLowInstances
                                          : frameUltraInstances) +=
                    chunk.counts[v];
            }
        }
    }
}

// Every (variant, level) batch must have its own group slot — an
// out-of-range group aliases another variant's command range and the
// prop blinks with the ping-pong (the kFirstDebris+1 postmortem).
static_assert(VegetationSystem::kGroupBase +
                  VegetationSystem::kVariantCount * 3 +
                  VegetationSystem::kTreeVariants <=
              GpuOcclusion::kMaxGroups);

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
            // THE level pick (canopyLevel) — shared with draw() so the
            // GPU-driven and CPU verdicts cannot drift; 3 = the
            // hero-near twin, group slot appended past the 3-level
            // block so the existing indexing never moves.
            const VariantMesh& mesh = variantMeshes[v];
            const u32 level = canopyLevel(
                mesh, v, cheb,
                cheb <= 1 && chunkNear(cx, cz, cameraPos),
                /*forceLowDetail=*/false);
            const u32 group =
                level == 3 ? kGroupBase + kVariantCount * 3 + v
                           : kGroupBase + v * 3 + level;
            out.push_back({ lo, hi, group, levelMesh(mesh, level).indexCount,
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
    bool albedoBound = false;
    for (u32 v = 0; v < kVariantCount; ++v) {
        const VariantMesh& mesh = variantMeshes[v];
        const ColonizedTreeParams& bp = barkParamsFor(v);
        const BarkPush push { Vec4(bp.barkTint, bp.barkTileScale),
                              Vec4(bp.barkHexCell, bp.barkHexSharpness,
                                   0.0f, 0.0f) };
        cmd.setPushConstants(&push, sizeof(push));
        if (mesh.albedoGroup.get().id != 0) {
            cmd.setBindGroup(1, mesh.albedoGroup.get());
            albedoBound = true;
        } else if (albedoBound) {
            cmd.setBindGroup(1, leafMaskGroup);
            albedoBound = false;
        }
        const u32 levels = mesh.lowIndexCount == 0
                               ? 1u
                               : mesh.ultraIndexCount == 0 ? 2u
                               : mesh.nearIndexCount == 0  ? 3u
                                                           : 4u;
        for (u32 level = 0; level < levels; ++level) {
            const u32 group = level == 3
                                  ? kGroupBase + kVariantCount * 3 + v
                                  : kGroupBase + v * 3 + level;
            if (groupCount[group] == 0) {
                continue;
            }
            const LevelMesh lod = levelMesh(mesh, level);
            cmd.setVertexBuffer(0, lod.vertices);
            cmd.setIndexBuffer(lod.indices, rhi::IndexFormat::U32);
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
        if (v >= kFirstPlant) {
            continue; // plants cast no shadows (cutout accents — GoT
                      // model; their fill-rate stays out of the cascades)
        }
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
