#include "engine/render/landscape/CliffSystem.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/Hash.hpp"
#include "engine/core/Log.hpp"
#include "engine/render/MeshVertexLayout.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {

constexpr const char* kCliffShader = "cliff";
constexpr const char* kCasterShader = "shadow_terrain";

// Ribbon resolution: column spacing along the band, target row step up
// the face. The relief detail lives in the material (triplanar +
// normal + SSDM) — this grid only carries the strata silhouette.
constexpr f32 kColumnStep = 4.0f;
constexpr f32 kRowStep = 2.5f;
constexpr u32 kMaxRows = 44; // tall faces stretch the row step instead
// Outward relief range (meters, along the node's horizontal dir). The
// floor keeps the drape proud of the heightfield (no z-fight); rows at
// the seams bury instead.
constexpr f32 kReliefFloor = 0.15f;
constexpr f32 kReliefAmp = 1.35f;
constexpr f32 kStrataPeriod = 7.0f; // meters of altitude per ledge

using core::hashU32;

f32 hash01(u32 v) {
    return static_cast<f32>(hashU32(v) & 0xffffu) / 65535.0f;
}

// Cheap value noise for the relief (worker-free build, deterministic).
f32 noise2(u32 seed, f32 u, f32 v) {
    const auto lattice = [&](i32 iu, i32 iv) {
        return hash01(seed ^ (static_cast<u32>(iu) * 668265263u) ^
                      (static_cast<u32>(iv) * 2246822519u));
    };
    const f32 fu = std::floor(u);
    const f32 fv = std::floor(v);
    const i32 iu = static_cast<i32>(fu);
    const i32 iv = static_cast<i32>(fv);
    const f32 tu = u - fu;
    const f32 tv = v - fv;
    const f32 a = glm::mix(lattice(iu, iv), lattice(iu + 1, iv), tu);
    const f32 b =
        glm::mix(lattice(iu, iv + 1), lattice(iu + 1, iv + 1), tu);
    return glm::mix(a, b, tv);
}

struct Column {
    Vec2 foot;    // xz
    Vec2 head;    // xz
    f32 footH;
    f32 headH;
    Vec2 dir;     // horizontal outward (downhill)
    f32 arc;      // along-band arclength (relief continuity)
};

} // namespace

void CliffSystem::create(rhi::Device& device, ShaderLibrary& shaders) {
    // Same closure as the terrain shader (shared vertex stage + splat
    // bind group); the caster reuses the terrain's depth-only pair.
    shaders.load(kCliffShader, { { "FrameUbo", 0 } },
                 { { "uSplat", 0 },
                   { "uShadowMap", 1 },
                   { "uSplatHeight", 3 },
                   { "uTerrainShade0", 4 },
                   { "uTerrainShade1", 5 },
                   { "uSplatNormal", 8 } },
                 "terrain");
    buildPipeline(device, shaders);
    buildCasterPipeline(device, shaders);
}

void CliffSystem::destroy(rhi::Device& device) {
    (void)device; // unique handles free through their device
    meshes.clear();
    pipeline.reset();
    casterPipeline.reset();
    baseKey = nullptr;
    walls = 0;
}

void CliffSystem::buildPipeline(rhi::Device& device,
                                ShaderLibrary& shaders) {
    pipeline = { device, device.createPipeline(
        { .shader = shaders.get(kCliffShader),
          .vertexBuffers = { meshVertexLayout() },
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Greater }, // reversed-Z
          .cull = rhi::CullMode::Back }) };
    shaderGeneration = shaders.generation(kCliffShader);
}

void CliffSystem::buildCasterPipeline(rhi::Device& device,
                                      ShaderLibrary& shaders) {
    if (shaders.get(kCasterShader).id == 0) {
        return; // TerrainSystem loads it in create() — order-guarded
    }
    casterPipeline = { device, device.createPipeline(
        { .shader = shaders.get(kCasterShader),
          .vertexBuffers = { meshVertexPositionLayout() },
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Less },
          .cull = rhi::CullMode::Back,
          .depthBias = 4.0f,
          .depthBiasSlope = 2.5f }) };
    casterShaderGeneration = shaders.generation(kCasterShader);
}

void CliffSystem::refreshPipeline(rhi::Device& device,
                                  ShaderLibrary& shaders) {
    if (shaders.generation(kCliffShader) != shaderGeneration) {
        buildPipeline(device, shaders);
    }
    if (casterPipeline.get().id == 0 ||
        shaders.generation(kCasterShader) != casterShaderGeneration) {
        buildCasterPipeline(device, shaders);
    }
}

void CliffSystem::update(rhi::Device& device, const TerrainParams& params) {
    const void* key =
        params.base ? static_cast<const void*>(params.base.get())
                    : nullptr;
    if (key == baseKey) {
        return;
    }
    baseKey = key;
    buildMeshes(device, params);
}

void CliffSystem::buildMeshes(rhi::Device& device,
                              const TerrainParams& params) {
    meshes.clear();
    walls = 0;
    if (!params.base) {
        return;
    }
    for (const TerrainRegion& region : params.base->regions) {
        if (region.cliffBands.empty()) {
            continue;
        }
        vector<MeshVertex> vertices;
        vector<u32> indices;
        RegionMesh mesh;
        const u32 regionSeed =
            hashU32(static_cast<u32>(
                        static_cast<i32>(region.originX * 0.01f)) ^
                    (static_cast<u32>(static_cast<i32>(
                         region.originZ * 0.01f))
                     << 16));
        for (const CliffBand& band : region.cliffBands) {
            if (band.nodes.size() < 2) {
                continue;
            }
            // --- Smoothed, subdivided columns -----------------------------
            // Node dirs averaged with their neighbours (a jagged foot
            // walk must not twist the wall), segments split to the
            // column step, arclength carried for relief continuity.
            vector<Column> columns;
            f32 arc = 0.0f;
            const auto nodeDir = [&](size_t i) {
                Vec2 dir { band.nodes[i].dirX, band.nodes[i].dirZ };
                if (i > 0) {
                    dir += Vec2 { band.nodes[i - 1].dirX,
                                  band.nodes[i - 1].dirZ };
                }
                if (i + 1 < band.nodes.size()) {
                    dir += Vec2 { band.nodes[i + 1].dirX,
                                  band.nodes[i + 1].dirZ };
                }
                const f32 len = glm::length(dir);
                return len > 1.0e-4f ? dir / len : Vec2 { 0.0f, 1.0f };
            };
            const auto columnOf = [&](size_t i) {
                const CliffNode& node = band.nodes[i];
                return Column { { node.x, node.z },
                                { node.headX, node.headZ },
                                node.footH,
                                node.headH,
                                nodeDir(i),
                                0.0f };
            };
            for (size_t i = 0; i + 1 < band.nodes.size(); ++i) {
                const Column a = columnOf(i);
                const Column b = columnOf(i + 1);
                const f32 segLen = glm::distance(
                    Vec2 { band.nodes[i].x, band.nodes[i].z },
                    Vec2 { band.nodes[i + 1].x, band.nodes[i + 1].z });
                const u32 divisions = glm::max(
                    1u, static_cast<u32>(segLen / kColumnStep));
                for (u32 d = 0; d < divisions; ++d) {
                    const f32 t = static_cast<f32>(d) /
                                  static_cast<f32>(divisions);
                    Column column;
                    column.foot = glm::mix(a.foot, b.foot, t);
                    column.head = glm::mix(a.head, b.head, t);
                    column.footH = glm::mix(a.footH, b.footH, t);
                    column.headH = glm::mix(a.headH, b.headH, t);
                    const Vec2 dir = glm::mix(a.dir, b.dir, t);
                    const f32 len = glm::length(dir);
                    column.dir =
                        len > 1.0e-4f ? dir / len : Vec2 { 0.0f, 1.0f };
                    column.arc = arc + segLen * t;
                    columns.push_back(column);
                }
                arc += segLen;
            }
            columns.push_back(columnOf(band.nodes.size() - 1));
            columns.back().arc = arc;
            if (columns.size() < 2) {
                continue;
            }

            // --- Row grid --------------------------------------------------
            f32 maxDrop = 0.0f;
            for (const Column& column : columns) {
                maxDrop = glm::max(maxDrop, column.headH - column.footH);
            }
            const f32 cover = glm::min(maxDrop, kMaxWallHeight);
            const u32 rows = glm::clamp(
                static_cast<u32>(cover / kRowStep), 3u, kMaxRows);
            const u32 firstVertex = static_cast<u32>(vertices.size());
            const u32 firstIndex = static_cast<u32>(indices.size());
            Vec3 lo { 1.0e9f };
            Vec3 hi { -1.0e9f };
            for (size_t c = 0; c < columns.size(); ++c) {
                const Column& column = columns[c];
                const f32 drop =
                    glm::min(column.headH - column.footH,
                             kMaxWallHeight);
                const bool endCap = c == 0 || c + 1 == columns.size();
                for (u32 r = 0; r <= rows; ++r) {
                    const f32 t = static_cast<f32>(r) /
                                  static_cast<f32>(rows);
                    // Drape point: the fall line between foot and the
                    // capped head, glued to the REAL terrain height.
                    const f32 reach =
                        drop / glm::max(column.headH - column.footH,
                                        1.0e-3f);
                    const Vec2 xz = glm::mix(
                        column.foot, column.head, t * reach);
                    const f32 ground =
                        terrain::height(params, xz.x, xz.y);
                    // Relief: strata ledges (altitude-banded, jittered
                    // along the band) + broad noise, always proud of
                    // the ground; seam rows bury instead.
                    const f32 bandY =
                        std::floor(ground / kStrataPeriod +
                                   0.35f * noise2(regionSeed ^ 0x11u,
                                                  column.arc * 0.06f,
                                                  0.0f));
                    const f32 ledge =
                        (hash01(regionSeed ^
                                static_cast<u32>(
                                    static_cast<i32>(bandY) * 7919)) -
                         0.5f) *
                        0.9f;
                    const f32 broad =
                        (noise2(regionSeed ^ 0x2fu, column.arc * 0.11f,
                                ground * 0.14f) -
                         0.5f) *
                        2.0f;
                    // Bigger faces carry bigger relief — 1.5 m of bumps
                    // on a 150 m wall is invisible from across the
                    // valley.
                    const f32 reliefScale =
                        1.0f + glm::min(drop, 120.0f) * 0.02f;
                    f32 relief = kReliefFloor +
                                 kReliefAmp * reliefScale *
                                     glm::clamp(0.55f + 0.5f * broad +
                                                    ledge,
                                                0.0f, 1.0f);
                    MeshVertex vertex;
                    vertex.position = { xz.x + column.dir.x * relief,
                                        ground,
                                        xz.y + column.dir.y * relief };
                    // Seams: bottom row buries under the talus, top row
                    // tucks into the crest, end columns into the hill.
                    if (r == 0) {
                        vertex.position.y -= 1.0f;
                    } else if (r == rows) {
                        vertex.position -=
                            Vec3 { column.dir.x, 0.0f, column.dir.y } *
                            (relief + 0.8f);
                        vertex.position.y -= 0.6f;
                    }
                    if (endCap && r > 0 && r < rows) {
                        vertex.position -=
                            Vec3 { column.dir.x, 0.0f, column.dir.y } *
                            (relief + 0.6f);
                    }
                    // Vertex mask: recesses darken, plus a broad
                    // per-strata tone roll (cliff.frag multiplies).
                    const f32 cavity = glm::clamp(
                        0.78f + 0.30f * (relief - 0.7f), 0.55f, 1.1f);
                    const f32 tone =
                        0.92f +
                        0.14f *
                            hash01(regionSeed ^
                                   static_cast<u32>(
                                       static_cast<i32>(bandY) * 271));
                    vertex.color = Vec3 { cavity * tone };
                    vertex.uv = { 0.0f, 0.0f };
                    lo = glm::min(lo, vertex.position);
                    hi = glm::max(hi, vertex.position);
                    vertices.push_back(vertex);
                }
            }
            const u32 stride = rows + 1;
            // The chain's travel direction along the foot line is
            // ARBITRARY (nearest-neighbour walk) — probe one mid-band
            // quad against the outward dir and pick the winding that
            // faces OUT, or back-face culling hides the whole wall.
            bool flip = false;
            {
                const size_t midC = columns.size() / 2;
                const u32 a = firstVertex +
                              static_cast<u32>(midC) * stride + rows / 2;
                const Vec3 face = glm::cross(
                    vertices[a + stride].position - vertices[a].position,
                    vertices[a + 1].position - vertices[a].position);
                const Vec2 dir = columns[midC].dir;
                flip = face.x * dir.x + face.z * dir.y < 0.0f;
            }
            for (u32 c = 0; c + 1 < columns.size(); ++c) {
                for (u32 r = 0; r < rows; ++r) {
                    const u32 a = firstVertex + c * stride + r;
                    const u32 b = a + stride;
                    if (flip) {
                        indices.insert(indices.end(),
                                       { a, a + 1, b, b, a + 1, b + 1 });
                    } else {
                        indices.insert(indices.end(),
                                       { a, b, a + 1, a + 1, b, b + 1 });
                    }
                }
            }
            // Accumulated smooth normals over the band's vertex range.
            for (size_t v = firstVertex; v < vertices.size(); ++v) {
                vertices[v].normal = Vec3 { 0.0f };
            }
            for (size_t i = firstIndex; i + 2 < indices.size(); i += 3) {
                MeshVertex& a = vertices[indices[i]];
                MeshVertex& b = vertices[indices[i + 1]];
                MeshVertex& c = vertices[indices[i + 2]];
                const Vec3 face = glm::cross(b.position - a.position,
                                             c.position - a.position);
                a.normal += face;
                b.normal += face;
                c.normal += face;
            }
            for (size_t v = firstVertex; v < vertices.size(); ++v) {
                const f32 len = glm::length(vertices[v].normal);
                vertices[v].normal = len > 1.0e-6f
                                         ? vertices[v].normal / len
                                         : Vec3 { 0.0f, 1.0f, 0.0f };
            }
            mesh.ranges.push_back(
                { firstIndex,
                  static_cast<u32>(indices.size()) - firstIndex,
                  lo - Vec3 { 1.0f }, hi + Vec3 { 1.0f } });
            ++walls;
        }
        if (vertices.empty() || indices.empty()) {
            continue;
        }
        mesh.vertexBuffer = { device, device.createBuffer(
            { .usage = rhi::BufferUsage::Vertex,
              .size = vertices.size() * sizeof(MeshVertex) },
            vertices.data()) };
        mesh.indexBuffer = { device, device.createBuffer(
            { .usage = rhi::BufferUsage::Index,
              .size = indices.size() * sizeof(u32) },
            indices.data()) };
        meshes.push_back(std::move(mesh));
    }
    if (walls > 0) {
        LOG_INFO("CliffSystem: {} wall(s) over {} region(s)", walls,
                 meshes.size());
    }
}

void CliffSystem::draw(rhi::CommandBuffer& cmd,
                       rhi::BindGroupHandle frameBindGroup,
                       rhi::BindGroupHandle splatBindGroup,
                       rhi::BindGroupHandle shadowBindGroup,
                       const Frustum* frustum) {
    if (meshes.empty() || pipeline.get().id == 0) {
        return;
    }
    cmd.setPipeline(pipeline);
    cmd.setBindGroup(0, frameBindGroup);
    if (splatBindGroup.id != 0) {
        cmd.setBindGroup(1, splatBindGroup);
    }
    if (shadowBindGroup.id != 0) {
        cmd.setBindGroup(2, shadowBindGroup);
    }
    for (const RegionMesh& mesh : meshes) {
        bool bound = false;
        for (const BandRange& range : mesh.ranges) {
            if (frustum != nullptr &&
                !frustum->intersectsAabb(range.lo, range.hi)) {
                continue;
            }
            if (!bound) {
                cmd.setVertexBuffer(0, mesh.vertexBuffer.get());
                cmd.setIndexBuffer(mesh.indexBuffer.get(),
                                   rhi::IndexFormat::U32);
                bound = true;
            }
            cmd.drawIndexed(range.indexCount, 1, range.firstIndex);
            frameIndices += range.indexCount;
        }
    }
}

void CliffSystem::drawDepth(rhi::CommandBuffer& cmd,
                            rhi::BindGroupHandle casterBindGroup,
                            const Frustum* frustum) {
    if (meshes.empty() || casterPipeline.get().id == 0) {
        return;
    }
    cmd.setPipeline(casterPipeline);
    cmd.setBindGroup(0, casterBindGroup);
    for (const RegionMesh& mesh : meshes) {
        bool bound = false;
        for (const BandRange& range : mesh.ranges) {
            if (frustum != nullptr &&
                !frustum->intersectsAabb(range.lo, range.hi)) {
                continue;
            }
            if (!bound) {
                cmd.setVertexBuffer(0, mesh.vertexBuffer.get());
                cmd.setIndexBuffer(mesh.indexBuffer.get(),
                                   rhi::IndexFormat::U32);
                bound = true;
            }
            cmd.drawIndexed(range.indexCount, 1, range.firstIndex);
            frameIndices += range.indexCount;
        }
    }
}

} // namespace render
