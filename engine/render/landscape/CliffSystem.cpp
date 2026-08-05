#include "engine/render/landscape/CliffSystem.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/quaternion.hpp>

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

using core::hashU32;
using core::HashRng;

f32 hash01(u32 v) {
    return static_cast<f32>(hashU32(v) & 0xffffu) / 65535.0f;
}

// Cheap value noise for the face displacement (deterministic).
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

// One displaced face sheet of a block: a grid over (axisU, axisV),
// displaced along `outward`, its OWN vertices (hard edges between the
// box faces — the "cut stone" read), smooth normals inside the sheet.
// Winding: (u, v) grid emitted so the front side faces `outward`.
void appendFace(MeshData& mesh, const CliffBlock& block,
                const glm::quat& rot, const Vec3& originL,
                const Vec3& axisU, f32 lenU, const Vec3& axisV,
                f32 lenV, const Vec3& outward, u32 cellsU, u32 cellsV,
                f32 dispAmp, f32 strataPeriod, f32 toneMul) {
    const u32 firstVertex = static_cast<u32>(mesh.vertices.size());
    const u32 vertsU = cellsU + 1;
    for (u32 v = 0; v <= cellsV; ++v) {
        for (u32 u = 0; u <= cellsU; ++u) {
            const f32 fu = static_cast<f32>(u) / static_cast<f32>(cellsU);
            const f32 fv = static_cast<f32>(v) / static_cast<f32>(cellsV);
            Vec3 local = originL + axisU * (fu * lenU) +
                         axisV * (fv * lenV);
            // De-cubing warp (continuous over the box: sheets stay
            // welded): top shrinks, the block shears, faces lose the
            // brick read.
            {
                const f32 y01 =
                    glm::clamp(local.y / block.height, 0.0f, 1.0f);
                const f32 shrink = 1.0f - block.taper * y01;
                local.x = local.x * shrink + block.skewX * local.y;
                local.z = local.z * shrink + block.skewZ * local.y;
            }
            f32 disp = 0.0f;
            f32 cavity = 0.6f;
            if (dispAmp > 0.0f) {
                // Strata terraces banded on the LOCAL height + broad
                // noise; the face rim stays put so the box edges keep
                // their crisp line.
                const f32 band =
                    std::floor((local.y + block.base.y) / strataPeriod);
                const f32 ledge =
                    (hash01(block.seed ^
                            static_cast<u32>(
                                static_cast<i32>(band) * 7919)) -
                     0.5f);
                const f32 broad =
                    noise2(block.seed, local.x * 0.16f + local.z * 0.11f,
                           local.y * 0.2f) -
                    0.5f;
                const f32 rim =
                    glm::min(glm::min(fu, 1.0f - fu),
                             glm::min(fv, 1.0f - fv));
                const f32 rimFade = glm::smoothstep(0.0f, 0.18f, rim);
                cavity = 0.5f + broad + ledge * 0.6f;
                disp = dispAmp * rimFade *
                       glm::clamp(cavity, 0.0f, 1.0f);
            }
            MeshVertex vertex;
            vertex.position =
                block.base + rot * (local + outward * disp);
            const f32 tone =
                toneMul * glm::mix(0.76f, 0.92f,
                                   glm::clamp(cavity, 0.0f, 1.0f));
            vertex.color = { tone, tone * 0.99f, tone * 0.965f };
            vertex.uv = { 0.0f, 0.0f };
            vertex.normal = Vec3 { 0.0f };
            mesh.vertices.push_back(vertex);
        }
    }
    for (u32 v = 0; v < cellsV; ++v) {
        for (u32 u = 0; u < cellsU; ++u) {
            const u32 a = firstVertex + v * vertsU + u;
            const u32 b = a + 1;
            const u32 c = a + vertsU;
            const u32 d = c + 1;
            // cross(axisU, axisV) == outward by the callers' choice of
            // axes, so (a, b, c) fronts outward.
            mesh.indices.insert(mesh.indices.end(),
                                { a, b, c, b, d, c });
        }
    }
    // Smooth normals inside the sheet only (hard box edges).
    for (size_t i = static_cast<size_t>(firstVertex);
         i < mesh.vertices.size(); ++i) {
        mesh.vertices[i].normal = Vec3 { 0.0f };
    }
    const size_t firstIndex =
        mesh.indices.size() -
        static_cast<size_t>(cellsU) * cellsV * 6;
    for (size_t i = firstIndex; i + 2 < mesh.indices.size(); i += 3) {
        MeshVertex& a = mesh.vertices[mesh.indices[i]];
        MeshVertex& b = mesh.vertices[mesh.indices[i + 1]];
        MeshVertex& c = mesh.vertices[mesh.indices[i + 2]];
        const Vec3 face = glm::cross(b.position - a.position,
                                     c.position - a.position);
        a.normal += face;
        b.normal += face;
        c.normal += face;
    }
    for (size_t i = static_cast<size_t>(firstVertex);
         i < mesh.vertices.size(); ++i) {
        const f32 len = glm::length(mesh.vertices[i].normal);
        mesh.vertices[i].normal =
            len > 1.0e-6f ? mesh.vertices[i].normal / len
                          : rot * outward;
    }
}

// The whole block: front + two sides + ledge top (back and bottom are
// buried). detail 1 = displaced strata faces; 0 = plain quads.
void appendBlock(MeshData& mesh, const CliffBlock& block, u32 detail) {
    const glm::quat rot =
        glm::angleAxis(block.yaw, Vec3 { 0.0f, 1.0f, 0.0f }) *
        glm::angleAxis(-block.lean, Vec3 { 1.0f, 0.0f, 0.0f }) *
        glm::angleAxis(block.roll, Vec3 { 0.0f, 0.0f, 1.0f });
    const f32 hw = block.width * 0.5f;
    const f32 h = block.height;
    const f32 d = block.depth;
    const f32 amp = detail == 0 ? 0.0f
                                : glm::clamp(0.5f + block.width * 0.03f,
                                             0.6f, 1.6f);
    const auto cells = [&](f32 len) {
        return detail == 0
                   ? 1u
                   : glm::clamp(static_cast<u32>(len / 6.0f), 2u, 4u);
    };
    // Front (+z), from (-hw, 0, 0): axisU = +x, axisV = +y, out = +z.
    appendFace(mesh, block, rot, { -hw, 0.0f, 0.0f },
               { 1.0f, 0.0f, 0.0f }, block.width, { 0.0f, 1.0f, 0.0f },
               h, { 0.0f, 0.0f, 1.0f }, cells(block.width), cells(h),
               amp, 6.5f, 0.96f);
    // Right side (+x): cross(-z, +y) = +x.
    appendFace(mesh, block, rot, { hw, 0.0f, 0.0f },
               { 0.0f, 0.0f, -1.0f }, d, { 0.0f, 1.0f, 0.0f }, h,
               { 1.0f, 0.0f, 0.0f }, cells(d), cells(h), amp * 0.7f,
               6.5f, 0.94f);
    // Left side (-x): cross(+z, +y) = -x.
    appendFace(mesh, block, rot, { -hw, 0.0f, -d },
               { 0.0f, 0.0f, 1.0f }, d, { 0.0f, 1.0f, 0.0f }, h,
               { -1.0f, 0.0f, 0.0f }, cells(d), cells(h), amp * 0.7f,
               6.5f, 0.94f);
    // Ledge top (+y): cross(+z, +x) = +y.
    appendFace(mesh, block, rot, { -hw, h, -d }, { 0.0f, 0.0f, 1.0f },
               d, { 1.0f, 0.0f, 0.0f }, block.width,
               { 0.0f, 1.0f, 0.0f }, cells(d), cells(block.width),
               amp * 0.5f, 6.5f, 1.0f);
}

} // namespace

u32 cliffRegionSeed(const TerrainRegion& region) {
    return hashU32(
        static_cast<u32>(static_cast<i32>(region.originX * 0.01f)) ^
        (static_cast<u32>(static_cast<i32>(region.originZ * 0.01f))
         << 16));
}

vector<CliffBlock> planCliffBlocks(const TerrainParams& params,
                                   const CliffBand& band,
                                   u32 regionSeed) {
    vector<CliffBlock> blocks;
    if (band.nodes.size() < 2) {
        return blocks;
    }
    // Stations along the foot polyline, one block RUN per station;
    // spacing slightly under the block width so neighbours interlock.
    f32 arc = 0.0f;
    f32 nextStation = 0.0f;
    for (size_t i = 0; i + 1 < band.nodes.size(); ++i) {
        const CliffNode& a = band.nodes[i];
        const CliffNode& b = band.nodes[i + 1];
        const f32 segLen =
            std::hypot(b.x - a.x, b.z - a.z);
        if (segLen < 1.0e-3f) {
            continue;
        }
        while (nextStation <= arc + segLen) {
            const f32 t = (nextStation - arc) / segLen;
            const f32 footX = glm::mix(a.x, b.x, t);
            const f32 footZ = glm::mix(a.z, b.z, t);
            const f32 footH = glm::mix(a.footH, b.footH, t);
            const f32 headX = glm::mix(a.headX, b.headX, t);
            const f32 headZ = glm::mix(a.headZ, b.headZ, t);
            const f32 headH = glm::mix(a.headH, b.headH, t);
            HashRng rng { hashU32(
                regionSeed ^
                static_cast<u32>(static_cast<i32>(footX * 7.31f)) ^
                (static_cast<u32>(static_cast<i32>(footZ * 5.17f))
                 << 12)) };
            const f32 wallH = glm::min(
                headH - footH, CliffSystem::kMaxWallHeight);
            // Face outward along the band's local dir; the block yaw
            // points its +z that way.
            const f32 dirX = glm::mix(a.dirX, b.dirX, t);
            const f32 dirZ = glm::mix(a.dirZ, b.dirZ, t);
            const f32 dirLen =
                glm::max(std::hypot(dirX, dirZ), 1.0e-4f);
            // R_y(yaw) maps +z to (sin yaw, 0, cos yaw) (glm quat).
            const f32 yaw =
                std::atan2(dirX / dirLen, dirZ / dirLen);
            const f32 width = 16.0f + rng.next() * 12.0f;
            // Terraces up the fall line: each block seats on the REAL
            // terrain at its station and rises past the next one — the
            // stepped crag profile.
            const u32 terraces = glm::clamp(
                static_cast<u32>(wallH / 22.0f), 1u, 3u);
            const f32 terraceH = wallH / static_cast<f32>(terraces);
            for (u32 terrace = 0; terrace < terraces; ++terrace) {
                const f32 tf = static_cast<f32>(terrace) /
                               static_cast<f32>(terraces);
                const f32 sx = glm::mix(footX, headX, tf);
                const f32 sz = glm::mix(footZ, headZ, tf);
                const f32 ground = terrain::height(params, sx, sz);
                CliffBlock block;
                block.width = width * (0.9f + rng.next() * 0.25f);
                block.height = glm::clamp(
                    terraceH * (1.15f + rng.next() * 0.35f), 8.0f,
                    30.0f);
                block.depth =
                    7.0f + block.height * 0.35f + rng.next() * 3.0f;
                block.yaw = yaw + (rng.next() - 0.5f) * 0.3f;
                block.lean = 0.05f + rng.next() * 0.12f;
                block.taper = 0.08f + rng.next() * 0.22f;
                block.skewX = (rng.next() - 0.5f) * 0.24f;
                block.skewZ = (rng.next() - 0.5f) * 0.12f;
                block.roll = (rng.next() - 0.5f) * 0.10f;
                block.seed = hashU32(regionSeed ^
                                     static_cast<u32>(blocks.size() *
                                                      2654435761u));
                // Seat sunk below its terrace ground, front face proud
                // of the slope by ~1 m.
                block.base = { sx + dirX / dirLen * 1.0f,
                               ground - 1.5f,
                               sz + dirZ / dirLen * 1.0f };
                blocks.push_back(block);
            }
            nextStation += width * 0.9f;
        }
        arc += segLen;
    }
    return blocks;
}

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
        const u32 regionSeed = cliffRegionSeed(region);
        MeshData nearMesh;
        MeshData farMesh;
        RegionMesh mesh;
        for (const CliffBand& band : region.cliffBands) {
            const vector<CliffBlock> blocks =
                planCliffBlocks(params, band, regionSeed);
            if (blocks.empty()) {
                continue;
            }
            BandRange range;
            range.firstIndex = static_cast<u32>(nearMesh.indices.size());
            range.farFirstIndex = static_cast<u32>(farMesh.indices.size());
            Vec3 lo { 1.0e9f };
            Vec3 hi { -1.0e9f };
            for (const CliffBlock& block : blocks) {
                appendBlock(nearMesh, block, 1);
                appendBlock(farMesh, block, 0);
                const f32 reach = block.width + block.depth +
                                  block.height;
                lo = glm::min(lo, block.base - Vec3 { reach });
                hi = glm::max(hi, block.base + Vec3 { reach });
            }
            range.indexCount =
                static_cast<u32>(nearMesh.indices.size()) - range.firstIndex;
            range.farIndexCount = static_cast<u32>(farMesh.indices.size()) -
                                  range.farFirstIndex;
            range.lo = lo;
            range.hi = hi;
            mesh.ranges.push_back(range);
            ++walls;
        }
        if (nearMesh.vertices.empty()) {
            continue;
        }
        mesh.vertexBuffer = { device, device.createBuffer(
            { .usage = rhi::BufferUsage::Vertex,
              .size = nearMesh.vertices.size() * sizeof(MeshVertex) },
            nearMesh.vertices.data()) };
        mesh.indexBuffer = { device, device.createBuffer(
            { .usage = rhi::BufferUsage::Index,
              .size = nearMesh.indices.size() * sizeof(u32) },
            nearMesh.indices.data()) };
        mesh.farVertexBuffer = { device, device.createBuffer(
            { .usage = rhi::BufferUsage::Vertex,
              .size = farMesh.vertices.size() * sizeof(MeshVertex) },
            farMesh.vertices.data()) };
        mesh.farIndexBuffer = { device, device.createBuffer(
            { .usage = rhi::BufferUsage::Index,
              .size = farMesh.indices.size() * sizeof(u32) },
            farMesh.indices.data()) };
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
                       const Vec3& cameraPos, const Frustum* frustum) {
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
        // Near and far ranges batch per buffer bind.
        for (u32 pass = 0; pass < 2; ++pass) {
            bool bound = false;
            for (const BandRange& range : mesh.ranges) {
                if (frustum != nullptr &&
                    !frustum->intersectsAabb(range.lo, range.hi)) {
                    continue;
                }
                const Vec3 center = (range.lo + range.hi) * 0.5f;
                const bool nearBand =
                    glm::max(glm::abs(center.x - cameraPos.x),
                             glm::abs(center.z - cameraPos.z)) <
                    kNearRange;
                if (nearBand != (pass == 0)) {
                    continue;
                }
                if (!bound) {
                    cmd.setVertexBuffer(
                        0, pass == 0 ? mesh.vertexBuffer.get()
                                     : mesh.farVertexBuffer.get());
                    cmd.setIndexBuffer(
                        pass == 0 ? mesh.indexBuffer.get()
                                  : mesh.farIndexBuffer.get(),
                        rhi::IndexFormat::U32);
                    bound = true;
                }
                cmd.drawIndexed(pass == 0 ? range.indexCount
                                          : range.farIndexCount,
                                1,
                                pass == 0 ? range.firstIndex
                                          : range.farFirstIndex);
                frameIndices += pass == 0 ? range.indexCount
                                          : range.farIndexCount;
            }
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
                cmd.setVertexBuffer(0, mesh.farVertexBuffer.get());
                cmd.setIndexBuffer(mesh.farIndexBuffer.get(),
                                   rhi::IndexFormat::U32);
                bound = true;
            }
            cmd.drawIndexed(range.farIndexCount, 1,
                            range.farFirstIndex);
            frameIndices += range.farIndexCount;
        }
    }
}

} // namespace render
