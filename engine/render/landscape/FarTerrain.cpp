#include "engine/render/landscape/FarTerrain.hpp"

#include "engine/core/Hash.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/render/MeshVertexLayout.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/render/landscape/TerrainSystem.hpp"
#include "engine/render/landscape/VegetationSystem.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {
constexpr const char* kFarTerrainShader = "far_terrain";
constexpr const char* kFarTreeShader = "far_tree";
constexpr Vec3 kForestTint { 0.14f, 0.23f, 0.11f };
} // namespace

void FarTerrain::create(rhi::Device& device, ShaderLibrary& shaders,
                        core::JobSystem& jobSystem) {
    jobs = &jobSystem;
    built = std::make_shared<core::ConcurrentQueue<Baked>>();
    shaders.load(kFarTerrainShader, { { "FrameUbo", 0 } },
                 { { "uCloudMap", 2 } });
    shaders.load(kFarTreeShader, { { "FrameUbo", 0 } },
                 { { "uCloudMap", 2 } });
    // The grid topology never changes — one static index buffer.
    vector<u32> indices;
    indices.reserve(static_cast<size_t>(kGridN) * kGridN * 6);
    for (u32 row = 0; row < kGridN; ++row) {
        for (u32 col = 0; col < kGridN; ++col) {
            const u32 a = row * (kGridN + 1) + col;
            const u32 b = a + 1;
            const u32 c = a + (kGridN + 1);
            const u32 d = c + 1;
            indices.insert(indices.end(), { a, c, b, b, c, d });
        }
    }
    indexCount = static_cast<u32>(indices.size());
    indexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = indices.size() * sizeof(u32) },
        indices.data()) };
    refreshPipeline(device, shaders);
}

void FarTerrain::destroy(rhi::Device& device) {
    (void)device;
    ++generation; // orphan in-flight bakes
    *this = FarTerrain {};
}

void FarTerrain::refreshPipeline(rhi::Device& device,
                                 ShaderLibrary& shaders) {
    if (pipeline.id() != 0 &&
        shaders.generation(kFarTerrainShader) +
                shaders.generation(kFarTreeShader) ==
            shaderGeneration) {
        return;
    }
    pipeline = { device, device.createPipeline(
        { .shader = shaders.get(kFarTerrainShader),
          .vertexBuffers = { meshVertexLayout() },
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Greater }, // reversed-Z
          .cull = rhi::CullMode::Back }) };
    // Impostor quads: corners from the vertex index, one instance
    // stream (the vegetation Instance layout) — no cull, a cylindrical
    // billboard's winding flips with the view side.
    treePipeline = { device, device.createPipeline(
        { .shader = shaders.get(kFarTreeShader),
          .vertexBuffers =
              { { .stride = sizeof(TreeInstance),
                  .stepMode = rhi::VertexStepMode::Instance,
                  .attributes =
                      { { .location = 0,
                          .format = rhi::VertexFormat::F32x4,
                          .offset = offsetof(TreeInstance, positionScale) },
                        { .location = 1,
                          .format = rhi::VertexFormat::F32x4,
                          .offset = offsetof(TreeInstance, params) } } } },
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Greater }, // reversed-Z
          .cull = rhi::CullMode::None }) };
    shaderGeneration = shaders.generation(kFarTerrainShader) +
                       shaders.generation(kFarTreeShader);
}

void FarTerrain::update(rhi::Device& device, const TerrainParams& params,
                        const Vec3& focus,
                        const VegetationSystem::TreeSilhouette& trees) {
    Baked done;
    while (built->tryPop(done)) {
        if (done.gen != generation) {
            continue;
        }
        vertexBuffer = { device, device.createBuffer(
            { .usage = rhi::BufferUsage::Vertex,
              .size = done.vertices.size() * sizeof(MeshVertex) },
            done.vertices.data()) };
        treeCount = static_cast<u32>(done.trees.size());
        treeBuffer = treeCount != 0
                         ? rhi::UniqueBuffer { device,
                                               device.createBuffer(
                                                   { .usage = rhi::BufferUsage::Vertex,
                                                     .size = done.trees.size() *
                                                             sizeof(TreeInstance) },
                                                   done.trees.data()) }
                         : rhi::UniqueBuffer {};
        bakedSeed = done.seed;
        bakedContentStamp = done.contentStamp;
        bakedSeaLevel = done.seaLevel;
        inFlight = false;
        uploaded = true;
    }
    if (inFlight) {
        return;
    }
    const Vec2 camXz { focus.x, focus.z };
    const bool stale = !uploaded ||
                       glm::distance(camXz, center) > kSpan * 0.08f ||
                       bakedSeed != params.seed ||
                       bakedSeaLevel != params.seaLevel ||
                       bakedContentStamp != params.contentStamp ||
                       std::abs(bakedTreeHeight - trees.height) > 0.5f;
    if (!stale) {
        return;
    }
    constexpr f32 kCell = kSpan / static_cast<f32>(kGridN);
    const Vec2 want = glm::floor(camXz / kCell) * kCell;
    center = want; // draw follows the request; the bake lands async
    bakedTreeHeight = trees.height;
    inFlight = true;
    jobs->enqueue([queue = built, params, want, trees,
                   gen = generation] {
        constexpr u32 kVertsN = kGridN + 1;
        constexpr f32 cell = kSpan / static_cast<f32>(kGridN);
        const f32 originX = want.x - kSpan * 0.5f;
        const f32 originZ = want.y - kSpan * 0.5f;
        Baked baked;
        baked.seed = params.seed;
        baked.seaLevel = params.seaLevel;
        baked.contentStamp = params.contentStamp;
        baked.gen = gen;
        // Heights on a HALF-CELL grid: the vertex takes the MIN over its
        // quad support so the 62 m linear interpolation can never rise
        // above the fine terrain (a fixed sink cannot — on a slope the
        // interpolation overshoot exceeds any constant, and the forest
        // canopy raise ate most of it: the poke-through triangles). The
        // vertex ALSO remembers what it gave up (true-height delta +
        // canopy raise) in uv.x; the shader restores it beyond the
        // streaming ring, where the far mesh is the only geometry and
        // the crests must keep their true silhouettes.
        constexpr u32 kHalfN = kGridN * 2 + 1;
        vector<f32> halfHeights(static_cast<size_t>(kHalfN) * kHalfN);
        for (u32 row = 0; row < kHalfN; ++row) {
            for (u32 col = 0; col < kHalfN; ++col) {
                halfHeights[static_cast<size_t>(row) * kHalfN + col] =
                    terrain::height(
                        params,
                        originX + static_cast<f32>(col) * cell * 0.5f,
                        originZ + static_cast<f32>(row) * cell * 0.5f);
            }
        }
        const auto halfAt = [&](i32 row, i32 col) {
            row = glm::clamp(row, 0, static_cast<i32>(kHalfN) - 1);
            col = glm::clamp(col, 0, static_cast<i32>(kHalfN) - 1);
            return halfHeights[static_cast<size_t>(row) * kHalfN + col];
        };
        const auto minAt = [&](i32 row, i32 col) {
            f32 h = 1e9f;
            for (i32 dr = -2; dr <= 2; ++dr) {
                for (i32 dc = -2; dc <= 2; ++dc) {
                    h = glm::min(h, halfAt(row * 2 + dr, col * 2 + dc));
                }
            }
            return h;
        };
        vector<f32> heights(static_cast<size_t>(kVertsN) * kVertsN);
        for (u32 row = 0; row < kVertsN; ++row) {
            for (u32 col = 0; col < kVertsN; ++col) {
                heights[static_cast<size_t>(row) * kVertsN + col] =
                    minAt(static_cast<i32>(row), static_cast<i32>(col));
            }
        }
        const auto heightAt = [&](i32 row, i32 col) {
            row = glm::clamp(row, 0, static_cast<i32>(kGridN));
            col = glm::clamp(col, 0, static_cast<i32>(kGridN));
            return heights[static_cast<size_t>(row) * kVertsN + col];
        };
        baked.vertices.resize(heights.size());
        for (u32 row = 0; row < kVertsN; ++row) {
            for (u32 col = 0; col < kVertsN; ++col) {
                const f32 x = originX + static_cast<f32>(col) * cell;
                const f32 z = originZ + static_cast<f32>(row) * cell;
                const f32 h = heightAt(row, col);
                const f32 trueH = halfAt(static_cast<i32>(row) * 2,
                                         static_cast<i32>(col) * 2);
                const Vec3 n = glm::normalize(Vec3 {
                    heightAt(row, col - 1) - heightAt(row, col + 1),
                    2.0f * cell,
                    heightAt(row - 1, col) - heightAt(row + 1, col) });
                // The shared forest mask + the real scatter's gates:
                // the fringe rises and darkens exactly where the true
                // trees grow, continuing them past the vegetation ring.
                f32 forest = forestMask(params.seed, x, z);
                const f32 slope = 1.0f - n.y;
                const f32 line = terrain::treeLine(params);
                // Same fade as the real scatter: the fringe thins out
                // over the last band below the treeline.
                forest *=
                    1.0f - glm::smoothstep(0.82f * line, line, trueH);
                if (trueH < params.seaLevel + 3.0f || slope > 0.3f) {
                    forest = 0.0f;
                }
                Vec3 color = terrainColor(trueH, n, params.seaLevel);
                color = glm::mix(color, kForestTint, forest * 0.85f);
                // Canopy mass = ~60% of the measured tree height (the
                // impostor crowns emerge above it) — applied by the
                // shader beyond the ring, with the true-height delta.
                baked.vertices[static_cast<size_t>(row) * kVertsN + col] =
                    MeshVertex { { x, h, z },
                                 n,
                                 { (trueH - h) +
                                       forest * (trees.height * 0.6f),
                                   0.0f },
                                 color };
            }
        }
        // Tree impostors: the real scatter's mask and gates, ~3.5x the
        // real spacing (silhouettes, not a forest sim), over the ring
        // where they can ever be visible.
        const i32 treeCells =
            static_cast<i32>(2.0f * kTreeFar / kTreeSpacing);
        baked.trees.reserve(4096);
        for (i32 tz = 0; tz < treeCells; ++tz) {
            for (i32 tx = 0; tx < treeCells; ++tx) {
                core::HashRng rng { core::hashU32(
                    params.seed ^ 0x51e57a7bu ^
                    core::hashU32(static_cast<u32>(tx * 73856093 ^
                                                   tz * 19349663))) };
                const f32 x = want.x - kTreeFar +
                              (static_cast<f32>(tx) + rng.next()) *
                                  kTreeSpacing;
                const f32 z = want.y - kTreeFar +
                              (static_cast<f32>(tz) + rng.next()) *
                                  kTreeSpacing;
                const f32 forest = forestMask(params.seed, x, z);
                if (forest < 0.05f || rng.next() >= forest * 0.6f) {
                    continue;
                }
                const f32 h = terrain::height(params, x, z);
                const Vec3 n = terrain::normal(params, x, z);
                const f32 line = terrain::treeLine(params);
                const f32 lineFade =
                    1.0f - glm::smoothstep(0.82f * line, line, h);
                if (h < params.seaLevel + 3.0f ||
                    rng.next() >= lineFade || (1.0f - n.y) > 0.3f) {
                    continue;
                }
                // Sized from the MEASURED real trees: the mesh
                // carries ~60% of that height as canopy mass, so the
                // billboards (85-115% of a real tree) emerge from it.
                baked.trees.push_back(
                    { { x, h - 0.5f, z,
                        trees.height * (0.85f + rng.next() * 0.3f) },
                      { rng.next() * 64.0f, rng.next(),
                        trees.widthRatio, trees.trunkFraction } });
            }
        }
        queue->push(std::move(baked));
    });
}

void FarTerrain::draw(rhi::CommandBuffer& cmd,
                      rhi::BindGroupHandle frameBindGroup,
                      rhi::BindGroupHandle cloudMapGroup) {
    if (!uploaded || pipeline.id() == 0) {
        return;
    }
    cmd.setPipeline(pipeline);
    cmd.setBindGroup(0, frameBindGroup);
    if (cloudMapGroup.id != 0) {
        cmd.setBindGroup(3, cloudMapGroup);
    }
    cmd.setVertexBuffer(0, vertexBuffer, 0);
    cmd.setIndexBuffer(indexBuffer, rhi::IndexFormat::U32);
    cmd.drawIndexed(indexCount);
    if (treeCount != 0 && treePipeline.id() != 0) {
        cmd.setPipeline(treePipeline);
        cmd.setBindGroup(0, frameBindGroup);
        if (cloudMapGroup.id != 0) {
            cmd.setBindGroup(3, cloudMapGroup);
        }
        cmd.setVertexBuffer(0, treeBuffer, 0);
        cmd.draw(6, treeCount);
    }
}

} // namespace render
