#include "engine/render/landscape/WaterRenderer.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"
#include "engine/render/PipelinePresets.hpp"

namespace render {

namespace {

constexpr const char* kWaterShader = "water";
constexpr const char* kWaterLocalShader = "waterlocal";
constexpr const char* kWaterSimShader = "water_sim";
constexpr const char* kWaterSimFrozenShader = "water_simfrozen";
constexpr const char* kWaterSimBoxShader = "water_simdbg";

// Worker-side pool-depth bake: vertical water column over the terrain
// (sea + lakes + river ribbons when bodies are set), then a separable
// max-dilation (~36 m) so one texture tap answers "how deep does this
// POOL get nearby" — the small-pool foam criterion.
vector<f32> bakePoolDepth(const TerrainParams& params,
                          const sptr<const WaterBodies>& bodies,
                          Vec2 center,
                          const sptr<const HeightField::Snapshot>& field) {
    constexpr u32 kSize = WaterRenderer::kPoolMapSize;
    constexpr i32 kDilate = 3; // texels (~36 m radius)

    vector<f32> depth(static_cast<size_t>(kSize) * kSize);
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const f32 wx = center.x +
                           (static_cast<f32>(x) / kSize - 0.5f) *
                               WaterRenderer::kPoolMapSpan;
            const f32 wz = center.y +
                           (static_cast<f32>(y) / kSize - 0.5f) *
                               WaterRenderer::kPoolMapSpan;
            // Shared grid at 12 m texels (dilated ~36 m after); exact
            // function without a snapshot.
            const f32 ground = field
                                   ? field->height(wx, wz)
                                   : terrain::height(params, wx, wz);
            depth[static_cast<size_t>(y) * kSize + x] =
                bodies ? terrain::waterDepthAt(*bodies, wx, wz, ground)
                       : glm::max(params.seaLevel - ground, 0.0f);
        }
    }
    // Separable max filter (horizontal then vertical).
    vector<f32> pass(depth.size());
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            f32 best = 0.0f;
            for (i32 dx = -kDilate; dx <= kDilate; ++dx) {
                const i32 sx = glm::clamp(static_cast<i32>(x) + dx, 0,
                                          static_cast<i32>(kSize) - 1);
                best = glm::max(best,
                                depth[static_cast<size_t>(y) * kSize +
                                      static_cast<u32>(sx)]);
            }
            pass[static_cast<size_t>(y) * kSize + x] = best;
        }
    }
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            f32 best = 0.0f;
            for (i32 dy = -kDilate; dy <= kDilate; ++dy) {
                const i32 sy = glm::clamp(static_cast<i32>(y) + dy, 0,
                                          static_cast<i32>(kSize) - 1);
                best = glm::max(best,
                                pass[static_cast<size_t>(sy) * kSize + x]);
            }
            depth[static_cast<size_t>(y) * kSize + x] = best;
        }
    }
    return depth;
}

// Unit quad in [-1,1]²; water.vert scales it around the camera and pins it
// to sea level. Flat geometry — the waves live in the fragment normals.
// CCW seen from ABOVE (+Y): the surface faces the sky.
constexpr f32 kQuadVertices[] = {
    -1.0f, -1.0f,
     1.0f, -1.0f,
     1.0f,  1.0f,
    -1.0f,  1.0f,
};
constexpr u16 kQuadIndices[] = { 0, 2, 1, 0, 3, 2 };

} // namespace

void WaterRenderer::create(rhi::Device& device, ShaderLibrary& shaders,
                           core::JobSystem& jobSystem) {
    poolBakes.create(jobSystem, "waterPoolMap");
    localBuilds.create(jobSystem, "waterLocalMesh");
    farBuilds.create(jobSystem, "farWater");

    vertexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex, .size = sizeof(kQuadVertices) },
        kQuadVertices) };
    indexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Index, .size = sizeof(kQuadIndices) },
        kQuadIndices) };

    poolMapSampler = { device, device.createSampler({}) }; // linear clamp
    // 1x1 "deep everywhere" placeholder until the first bake lands: foam
    // behaves as before for a second.
    const f32 kDeep = 10.0f;
    poolMap = { device, device.createTexture(
        { .width = 1,
          .height = 1,
          .format = rhi::TextureFormat::R16F,
          .usage = rhi::TextureUsage_Sampled },
        &kDeep) };
    const f32 kZeros[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    // Sim placeholder: dry everywhere until the first pre-roll lands
    // (created before the first bind-group build below).
    simMapB = { device, device.createTexture(
        { .width = 1,
          .height = 1,
          .format = rhi::TextureFormat::RGBA16F,
          .usage = rhi::TextureUsage_Sampled },
        kZeros) };
    // Unit cube for the debug volume boxes (8 verts, 12 tris).
    constexpr f32 kCubeVerts[] = {
        0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0,
        0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1,
    };
    constexpr u16 kCubeIndices[] = {
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
        3, 6, 2, 3, 7, 6, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5,
    };
    simBoxVertexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex, .size = sizeof(kCubeVerts) },
        kCubeVerts) };
    simBoxIndexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = sizeof(kCubeIndices) },
        kCubeIndices) };
    rebuildMaterials(device); // also builds the map bind group

    shaders.load(kWaterShader, { render::kFrameUbo },
                 { { "uSceneColor", 0 },
                   { "uSceneDepth", 1 },
                   { "uPoolDepth", 3 },
                   { "uSkyClouds", 4 },
                   { "uWaterSimB", 8 } });
    shaders.load(kWaterLocalShader,
                 { render::kFrameUbo, { "WaterMaterialsUbo", 1 } },
                 { { "uSceneColor", 0 },
                   { "uSceneDepth", 1 },
                   { "uPoolDepth", 3 },
                   { "uSkyClouds", 4 },
                   { "uWaterSimB", 8 } });
    shaders.load(kWaterSimShader,
                 { render::kFrameUbo, { "WaterMaterialsUbo", 1 } },
                 { { "uSceneColor", 0 },
                   { "uSceneDepth", 1 },
                   { "uPoolDepth", 3 },
                   { "uSkyClouds", 4 },
                   { "uWaterSimB", 8 } });
    shaders.load(kWaterSimFrozenShader,
                 { render::kFrameUbo, { "WaterMaterialsUbo", 1 } },
                 { { "uSceneColor", 0 },
                   { "uSceneDepth", 1 },
                   { "uPoolDepth", 3 },
                   { "uSkyClouds", 4 },
                   { "uWaterSimB", 8 } });
    shaders.load(kWaterSimBoxShader, { render::kFrameUbo }, {});
    refreshPipeline(device, shaders);
}

void WaterRenderer::destroy(rhi::Device& device) {
    (void)device;
    // The bind group goes first, before the textures and buffers it
    // references (the reset below destroys in declaration order).
    poolMapGroup.reset();
    // Identity survives the reset: the three lanes (their generation
    // keeps moving ACROSS the reset so every in-flight bake and mesh
    // build dies on arrival — a fresh zero would let them land as if
    // current). Everything else, GPU handles included, returns to the
    // default-constructed state, so a destroy()+create() cycle re-bakes
    // instead of trusting a pool map that no longer exists.
    core::BakeMailbox<BakedMap> savedPool = std::move(poolBakes);
    core::BakeMailbox<LocalMesh> savedLocal = std::move(localBuilds);
    core::BakeMailbox<FarMesh> savedFar = std::move(farBuilds);
    savedPool.reset();
    savedLocal.reset();
    savedFar.reset();
    *this = WaterRenderer {};
    poolBakes = std::move(savedPool);
    localBuilds = std::move(savedLocal);
    farBuilds = std::move(savedFar);
}

void WaterRenderer::setBodies(sptr<const WaterBodies> next) {
    bodies = std::move(next);
    ++bodiesStamp;
    bodiesDirty = true;
    // Nothing of the sim (crumb cache, frozen meshes) is touched here:
    // bodies change on EVERY streamed tile, and wiping wiped the water
    // memory every few seconds of travel (they only ever worked inside
    // fully streamed zones). Staleness is self-healing: pinLakes
    // re-rasterizes the pins from the CURRENT bodies on every resume
    // and scroll, and the frozen footprint refreshes on its timer.
    // The sculpt hook (a real ground change) keeps the full clear.
}

void WaterRenderer::kickLocalGeometry(const TerrainParams& params) {
    localBuilds.kick([bodiesRef = bodies, params, stamp = bodiesStamp](
                         LocalMesh& out, const std::atomic<bool>& stop) {
        out.stamp = stamp;
        if (!stop.load(std::memory_order_relaxed) && bodiesRef &&
            !(bodiesRef->lakes.empty() && bodiesRef->rivers.empty())) {
            terrain::buildLocalWaterGeometry(*bodiesRef, params,
                                             out.verts, out.indices);
        }
    });
}

void WaterRenderer::update(rhi::Device& device, const TerrainParams& params,
                           const Vec3& cameraPos,
                           sptr<const HeightField::Snapshot> field) {
    // Apply a finished bake: new texture + bind group (rebakes are rare —
    // every ~500 m of travel or on a settings change).
    poolBakes.drain([&](BakedMap& baked) {
        poolMap = { device, device.createTexture(
            { .width = kPoolMapSize,
              .height = kPoolMapSize,
              .format = rhi::TextureFormat::R16F,
              .filter = rhi::FilterMode::Linear,
              .usage = rhi::TextureUsage_Sampled },
            baked.texels.data()) };
        rebuildMapGroup(device);
        mapCenter = baked.center;
        bakedSeed = baked.seed;
        bakedSeaLevel = baked.seaLevel;
        bakedBodiesStamp = baked.bodiesStamp;
        bakedContentStamp = baked.contentStamp;
    });

    // Local geometry: WORKER-built (the synchronous build stuttered
    // the frame on every streamed tile — see LocalMesh); main uploads
    // the finished buffers and re-kicks if the bodies moved on.
    {
        localBuilds.drain([&](LocalMesh& lm) {
            localIndexBuffer.reset();
            localVertexBuffer.reset();
            localIndexCount = 0;
            if (!lm.verts.empty() && !lm.indices.empty()) {
                localVertexBuffer = { device, device.createBuffer(
                    { .usage = rhi::BufferUsage::Vertex,
                      .size = lm.verts.size() * sizeof(f32) },
                    lm.verts.data()) };
                localIndexBuffer = { device, device.createBuffer(
                    { .usage = rhi::BufferUsage::Index,
                      .size = lm.indices.size() * sizeof(u32) },
                    lm.indices.data()) };
                localIndexCount = static_cast<u32>(lm.indices.size());
            }
            if (lm.stamp != bodiesStamp) {
                bodiesDirty = true; // superseded mid-build: go again
            }
        });
    }
    if (bodiesDirty && !localBuilds.busy()) {
        rebuildMaterials(device);
        kickLocalGeometry(params);
        bodiesDirty = false;
    }

    // Far water: worker-built like the local mesh. Stale when the
    // camera strays ~2 km or a tile publish bumped contentStamp (a
    // fresh .twb may have replaced a master-network course).
    {
        farBuilds.drain([&](FarMesh& fm) {
            farIndexBuffer.reset();
            farVertexBuffer.reset();
            farIndexCount = 0;
            if (!fm.verts.empty() && !fm.indices.empty()) {
                farVertexBuffer = { device, device.createBuffer(
                    { .usage = rhi::BufferUsage::Vertex,
                      .size = fm.verts.size() * sizeof(f32) },
                    fm.verts.data()) };
                farIndexBuffer = { device, device.createBuffer(
                    { .usage = rhi::BufferUsage::Index,
                      .size = fm.indices.size() * sizeof(u32) },
                    fm.indices.data()) };
                farIndexCount = static_cast<u32>(fm.indices.size());
            }
            farCenter = fm.center;
            farStamp = fm.stamp;
        });
        const Vec2 farCam { cameraPos.x, cameraPos.z };
        // The 18 km collect rescans the tile cache on disk — once per
        // publish BURST, after a second of stamp quiet, not once per tile.
        const bool farContentSettled =
            farContent.settled(params.contentStamp, 1.0);
        if (farFn && !farBuilds.busy() &&
            (farDirty || glm::distance(farCam, farCenter) > 2048.0f ||
             (farStamp != static_cast<u64>(params.contentStamp) &&
              farContentSettled))) {
            farDirty = false;
            farBuilds.kick([fn = farFn, farCam,
                            stamp = static_cast<u64>(params.contentStamp)](
                               FarMesh& out, const std::atomic<bool>& stop) {
                out.stamp = stamp;
                out.center = farCam;
                if (!stop.load(std::memory_order_relaxed)) {
                    const FarWaterSet set =
                        fn(farCam.x, farCam.y, 9000.0f);
                    terrain::buildFarWaterGeometry(set, out.verts,
                                                   out.indices);
                }
            });
        }
    }

    const Vec2 camXz { cameraPos.x, cameraPos.z };
    // Terrain changes are rect-scoped: a tile published outside the
    // 3 km pool window no longer rebakes it. Bodies changes stay a
    // global signal (the resident list itself moved).
    const bool stale =
        glm::distance(camXz, mapCenter) > kRebakeDistance ||
        bakedSeed != params.seed || bakedSeaLevel != params.seaLevel ||
        bakedBodiesStamp != bodiesStamp ||
        terrain::contentTouchedSince(params, bakedContentStamp,
                                     mapCenter.x - kPoolMapSpan * 0.5f,
                                     mapCenter.y - kPoolMapSpan * 0.5f,
                                     mapCenter.x + kPoolMapSpan * 0.5f,
                                     mapCenter.y + kPoolMapSpan * 0.5f);
    if (stale && !poolBakes.busy()) {
        constexpr f32 kTexel = kPoolMapSpan / kPoolMapSize;
        const Vec2 center = glm::floor(camXz / kTexel) * kTexel;
        poolBakes.kick([params, center, bodiesRef = bodies,
                        stamp = bodiesStamp, content = params.contentStamp,
                        fieldRef = field](BakedMap& out,
                                          const std::atomic<bool>&) {
            out.center = center;
            out.seed = params.seed;
            out.seaLevel = params.seaLevel;
            out.bodiesStamp = stamp;
            out.contentStamp = content;
            out.texels = bakePoolDepth(params, bodiesRef, center, fieldRef);
        });
    }

}

void WaterRenderer::applySimRequests(
    rhi::Device& device,
    const vector<terrain::WaterSimControl::GpuRequest>& requests,
    const terrain::WaterSimControl::SimConfig& cfg) {
    using Kind = terrain::WaterSimControl::GpuRequest::Kind;
    for (const auto& req : requests) {
        switch (req.kind) {
        case Kind::ClearFrozenMeshes:
            simFrozenClearNow();
            break;
        case Kind::FreezeMesh:
            simFreeze(device, *req.snap, req.replaceRadius,
                      req.pushIfNoMatch);
            break;
        case Kind::UploadWindow:
            uploadSimTextures(device, *req.snap, cfg.debugMode);
            break;
        }
    }
}

void WaterRenderer::uploadSimTextures(rhi::Device& device,
                                      const terrain::WaterSimSnapshot& snap,
                                      i32 debugMode) {
    // Destroy+create each tick (the proven info-map path; both
    // backends defer deletion). An updateTexture RHI fast path is a
    // known later optimization.
    const u32 n = snap.spec.n;
    vector<f32> extras(static_cast<size_t>(n) * n * 4);
    for (size_t i = 0; i < snap.depth.size(); ++i) {
        extras[i * 4 + 0] = snap.depth[i];
        extras[i * 4 + 1] = snap.velX[i];
        extras[i * 4 + 2] = snap.velZ[i];
        extras[i * 4 + 3] = 0.0f;
    }
    simMapB = { device, device.createTexture(
        { .width = n,
          .height = n,
          .format = rhi::TextureFormat::RGBA16F,
          .filter = rhi::FilterMode::Linear,
          .usage = rhi::TextureUsage_Sampled },
        extras.data()) };
    rebuildMapGroup(device);

    // --- The ONE closed mesh, built worker-side in extractSnapshot
    // (docs/PAYSAGE.md §1.5): upload verbatim.
    simVertexBuffer.reset();
    simIndexBuffer.reset();
    simIndexCount = 0;
    if (!snap.meshIndices.empty()) {
        simVertexBuffer = { device, device.createBuffer(
            { .usage = rhi::BufferUsage::Vertex,
              .size = snap.meshVerts.size() * sizeof(f32) },
            snap.meshVerts.data()) };
        simIndexBuffer = { device, device.createBuffer(
            { .usage = rhi::BufferUsage::Index,
              .size = snap.meshIndices.size() * sizeof(u32) },
            snap.meshIndices.data()) };
        simIndexCount = static_cast<u32>(snap.meshIndices.size());
    }

    // --- Debug volume boxes: only while the mode shows them.
    simBoxInstanceBuffer.reset();
    simBoxInstances = 0;
    if (debugMode == 3) {
        const auto& spec = snap.spec;
        const f32 texel = spec.texelSize;
        vector<f32> cells;
        cells.reserve(4096);
        for (u32 row = 0; row < spec.n; ++row) {
            for (u32 col = 0; col < spec.n; ++col) {
                const size_t i = static_cast<size_t>(row) * spec.n + col;
                const f32 d = snap.depth[i];
                if (d <= 0.0f) {
                    continue;
                }
                cells.push_back(spec.originX +
                                static_cast<f32>(col) * texel);
                cells.push_back(snap.surface[i] - d);
                cells.push_back(spec.originZ +
                                static_cast<f32>(row) * texel);
                cells.push_back(d);
                if (cells.size() >= 4u * 80000u) {
                    break;
                }
            }
        }
        if (!cells.empty()) {
            simBoxInstanceBuffer = { device, device.createBuffer(
                { .usage = rhi::BufferUsage::Vertex,
                  .size = cells.size() * sizeof(f32) },
                cells.data()) };
            simBoxInstances = static_cast<u32>(cells.size() / 4);
        }
    }
}

void WaterRenderer::simFreeze(rhi::Device& device,
                              const terrain::WaterSimSnapshot& snap,
                              f32 replaceRadius, bool pushIfNoMatch) {
    if (snap.meshIndices.empty() || snap.spec.n < 2) {
        return;
    }
    const f32 span =
        static_cast<f32>(snap.spec.n - 1) * snap.spec.texelSize;
    // One entry per footprint: an entry within replaceRadius is
    // superseded (same zone, staler water) — mirrors the sim control's
    // simCachePush.
    bool matched = false;
    for (size_t i = 0; i < simFrozen.size(); ++i) {
        if (std::abs(simFrozen[i].span - span) < 1.0e-3f &&
            std::abs(simFrozen[i].originX - snap.spec.originX) <
                replaceRadius &&
            std::abs(simFrozen[i].originZ - snap.spec.originZ) <
                replaceRadius) {
            // Erasing the entry destroys its buffers.
            simFrozen.erase(simFrozen.begin() + static_cast<i32>(i));
            matched = true;
            break;
        }
    }
    if (!matched && !pushIfNoMatch) {
        return; // refresh-only call: never grow, never evict the trail
    }
    if (simFrozen.size() >= terrain::WaterSimControl::kSimCacheCap) {
        simFrozen.erase(simFrozen.begin());
    }
    FrozenWindow fz;
    fz.vertexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = snap.meshVerts.size() * sizeof(f32) },
        snap.meshVerts.data()) };
    fz.indexBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = snap.meshIndices.size() * sizeof(u32) },
        snap.meshIndices.data()) };
    fz.indexCount = static_cast<u32>(snap.meshIndices.size());
    fz.originX = snap.spec.originX;
    fz.originZ = snap.spec.originZ;
    fz.span = span;
    simFrozen.push_back(std::move(fz));
}

void WaterRenderer::simFrozenClearNow() {
    simFrozen.clear(); // destroys every frozen mesh's buffers
}

std::array<Vec4, 4> WaterRenderer::simFrozenInfo(i32 debugMode) const {
    std::array<Vec4, 4> out {};
    if (debugMode == 1) {
        return out; // force baked: meshes hidden, the baked never yields
    }
    const size_t count = glm::min(simFrozen.size(), out.size());
    for (size_t i = 0; i < count; ++i) {
        const FrozenWindow& fz = simFrozen[i];
        out[i] = { fz.originX, fz.originZ,
                   fz.span > 0.0f ? 1.0f / fz.span : 0.0f, 0.0f };
    }
    return out;
}

void WaterRenderer::rebuildMapGroup(rhi::Device& device) {
    poolMapGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 1, .buffer = materialsUbo },
                       { .binding = 3,
                         .texture = poolMap,
                         .sampler = poolMapSampler },
                       // Bindings 5/6 (WaterInfoMap) and 7 (the sim
                       // display plane, never read) are retired —
                       // frozen, never renumbered.
                       { .binding = 8,
                         .texture = simMapB,
                         .sampler = poolMapSampler } } }) };
}

void WaterRenderer::rebuildMaterials(rhi::Device& device) {
    // std140 mirror of water_surface.glsl's WaterMaterial (5 vec4 + 1).
    struct GpuWaterMaterial {
        Vec4 tintStrength;
        Vec4 deepEmissive;
        Vec4 absorptionFlow;
        Vec4 foamWave;
        Vec4 emissiveViscosity;
        Vec4 extras;
    };
    vector<GpuWaterMaterial> table(kMaxWaterMaterials);
    const WaterMaterialParams kDefault;
    for (u32 i = 0; i < kMaxWaterMaterials; ++i) {
        const WaterMaterialParams& m =
            (bodies && i < bodies->materials.size())
                ? bodies->materials[i]
                : kDefault;
        table[i] = { Vec4 { m.tint, m.tintStrength },
                     Vec4 { m.deepColor, m.emissiveStrength },
                     Vec4 { m.absorption, m.flowSpeedScale },
                     Vec4 { m.foamColor, m.waveScale },
                     Vec4 { m.emissiveColor, m.viscosity },
                     Vec4 { m.foamGain, 0.0f, 0.0f, 0.0f } };
    }
    materialsUbo = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Uniform,
          .size = table.size() * sizeof(GpuWaterMaterial) },
        table.data()) };
    rebuildMapGroup(device);
}

void WaterRenderer::refreshPipeline(rhi::Device& device,
                                    ShaderLibrary& shaders) {
    // One recipe per pipeline: each names the shader it consumes, so a
    // reload of that shader rebuilds exactly the pipelines built on it.
    // The sim mesh layout (pos F32x3 + node uv F32x2) is shared by the
    // live, frozen and overlay pipelines.
    const rhi::VertexBufferLayout kSimMeshLayout {
        .stride = 5 * sizeof(f32),
        .attributes = { { .location = 0,
                          .format = rhi::VertexFormat::F32x3,
                          .offset = 0 },
                        { .location = 1,
                          .format = rhi::VertexFormat::F32x2,
                          .offset = 3 * sizeof(f32) } } };
    pipeline.ensure(device, shaders, [&] {
        return device.createPipeline(
            { .shader = shaders.get(kWaterShader),
              .vertexBuffers =
                  { { .stride = 2 * sizeof(f32),
                      .attributes = { { .location = 0,
                                        .format = rhi::VertexFormat::F32x2,
                                        .offset = 0 } } } },
              // Opaque (refraction is composed manually from the scene
              // snapshot); depth-tested against the opaque pass, and
              // written so fog-of-depth effects later stay consistent.
              // Two-sided: the shader renders a distinct underside when
              // seen from below. Toward-camera bias (positive under
              // reversed-Z + Greater): where the sheet lies centimetres
              // over the ground (shallow shelves, flooded meadow dips)
              // the two planes can sit inside the depth-quantization
              // noise and the surface flickered pixel by pixel —
              // contour-line moiré fringes. The bias rides the depth
              // format's local precision, so it stays microscopic
              // everywhere reversed-Z keeps precision healthy.
              .depth = render::kDepthOpaque,
              .cull = rhi::CullMode::None,
              .depthBias = render::kDepthBias,
              .depthBiasSlope = render::kDepthBiasSlope });
    });
    // Local surfaces: same states, world-space F32x3 vertices.
    localPipeline.ensure(device, shaders, [&] {
        return device.createPipeline(
            { .shader = shaders.get(kWaterLocalShader),
              .vertexBuffers =
                  { { .stride = 10 * sizeof(f32),
                      .attributes = { { .location = 0,
                                        .format = rhi::VertexFormat::F32x3,
                                        .offset = 0 },
                                      { .location = 1,
                                        .format = rhi::VertexFormat::F32x2,
                                        .offset = 3 * sizeof(f32) },
                                      { .location = 2,
                                        .format = rhi::VertexFormat::F32x4,
                                        .offset = 5 * sizeof(f32) },
                                      { .location = 3,
                                        .format = rhi::VertexFormat::F32x1,
                                        .offset = 9 * sizeof(f32) } } } },
              .depth = render::kDepthOpaque,
              .cull = rhi::CullMode::None,
              .depthBias = render::kDepthBias,
              .depthBiasSlope = render::kDepthBiasSlope });
    });
    // Sim window: node-uv grid displaced by the display texture in the
    // vertex shader, same states and shared shading.
    simPipeline.ensure(device, shaders, [&] {
        return device.createPipeline(
            { .shader = shaders.get(kWaterSimShader),
              .vertexBuffers = { kSimMeshLayout },
              .depth = render::kDepthOpaque,
              .cull = rhi::CullMode::None,
              .depthBias = render::kDepthBias,
              .depthBiasSlope = render::kDepthBiasSlope });
    });
    // Frozen windows: same mesh layout and states, frozen shading.
    simFrozenPipeline.ensure(device, shaders, [&] {
        return device.createPipeline(
            { .shader = shaders.get(kWaterSimFrozenShader),
              .vertexBuffers = { kSimMeshLayout },
              .depth = render::kDepthOpaque,
              .cull = rhi::CullMode::None,
              .depthBias = render::kDepthBias,
              .depthBiasSlope = render::kDepthBiasSlope });
    });
    // Seam overlay: depth test OFF — the diagnosis view.
    simPipelineOverlay.ensure(device, shaders, [&] {
        return device.createPipeline(
            { .shader = shaders.get(kWaterSimShader),
              .vertexBuffers = { kSimMeshLayout },
              .depth = render::kDepthOff,
              .cull = rhi::CullMode::None });
    });
    // Debug volume boxes: instanced translucent columns.
    simBoxPipeline.ensure(device, shaders, [&] {
        return device.createPipeline(
            { .shader = shaders.get(kWaterSimBoxShader),
              .vertexBuffers =
                  { { .stride = 3 * sizeof(f32),
                      .attributes = { { .location = 0,
                                        .format = rhi::VertexFormat::F32x3,
                                        .offset = 0 } } },
                    { .stride = 4 * sizeof(f32),
                      .stepMode = rhi::VertexStepMode::Instance,
                      .attributes = { { .location = 1,
                                        .format = rhi::VertexFormat::F32x4,
                                        .offset = 0 } } } },
              .blend = rhi::BlendMode::Alpha,
              .depth = render::kDepthReadOnly,
              .cull = rhi::CullMode::None });
    });
}

void WaterRenderer::draw(rhi::CommandBuffer& cmd,
                         rhi::BindGroupHandle frameBindGroup,
                         rhi::BindGroupHandle sceneBindGroup,
                         const terrain::WaterSimControl& sim) {
    const i32 debugMode = sim.simConfig().debugMode;
    cmd.setPipeline(pipeline);
    cmd.setBindGroup(0, frameBindGroup);
    cmd.setBindGroup(1, sceneBindGroup);
    cmd.setBindGroup(2, poolMapGroup);
    cmd.setVertexBuffer(0, vertexBuffer);
    cmd.setIndexBuffer(indexBuffer, rhi::IndexFormat::U16);
    cmd.drawIndexed(6);
    if (localIndexCount > 0) {
        // Lakes at their own level + river ribbons, same shading.
        cmd.setPipeline(localPipeline);
        cmd.setBindGroup(0, frameBindGroup);
        cmd.setBindGroup(1, sceneBindGroup);
        cmd.setBindGroup(2, poolMapGroup);
        cmd.setVertexBuffer(0, localVertexBuffer);
        cmd.setIndexBuffer(localIndexBuffer, rhi::IndexFormat::U32);
        cmd.drawIndexed(localIndexCount);
    }
    if (farIndexCount > 0) {
        // Far water: sheets beyond the streamed tiles — same pipeline;
        // wherever near water exists at this spot, its depth-written
        // surface wins and the far fragment is culled.
        cmd.setPipeline(localPipeline);
        cmd.setBindGroup(0, frameBindGroup);
        cmd.setBindGroup(1, sceneBindGroup);
        cmd.setBindGroup(2, poolMapGroup);
        cmd.setVertexBuffer(0, farVertexBuffer);
        cmd.setIndexBuffer(farIndexBuffer, rhi::IndexFormat::U32);
        cmd.drawIndexed(farIndexCount);
    }
    if (sim.simIsLive() && sim.simIsValid() && simIndexCount > 0 &&
        debugMode != 1) {
        // The live sim sheet (drawn last: its fragments discard where
        // dry, the baked ones discard where the sim owns the texel).
        // Settle gate: while settling, only the baked water shows
        // (simMapInfo already reads invalid) — the mesh stays hidden
        // with it. Seam overlay renders WITHOUT depth test: the
        // unambiguous "where does the sim have water" view.
        cmd.setPipeline(debugMode == 2 ? simPipelineOverlay : simPipeline);
        cmd.setBindGroup(0, frameBindGroup);
        cmd.setBindGroup(1, sceneBindGroup);
        cmd.setBindGroup(2, poolMapGroup);
        cmd.setVertexBuffer(0, simVertexBuffer);
        cmd.setIndexBuffer(simIndexBuffer, rhi::IndexFormat::U32);
        cmd.drawIndexed(simIndexCount);
    }
    if (!simFrozen.empty() && debugMode != 1) {
        // Frozen windows: static past-sim meshes along the travel
        // trail (their shader yields inside the live window and to
        // fresher frozen rects; the baked yields inside them).
        cmd.setPipeline(simFrozenPipeline);
        cmd.setBindGroup(0, frameBindGroup);
        cmd.setBindGroup(1, sceneBindGroup);
        cmd.setBindGroup(2, poolMapGroup);
        for (const FrozenWindow& fz : simFrozen) {
            cmd.setVertexBuffer(0, fz.vertexBuffer);
            cmd.setIndexBuffer(fz.indexBuffer, rhi::IndexFormat::U32);
            cmd.drawIndexed(fz.indexCount);
        }
    }
    if (debugMode == 3 && simBoxInstances > 0) {
        // Debug volume columns of the LIVE window: "where the water IS"
        // (independent of any frozen window existing).
        cmd.setPipeline(simBoxPipeline);
        cmd.setBindGroup(0, frameBindGroup);
        cmd.setVertexBuffer(0, simBoxVertexBuffer);
        cmd.setVertexBuffer(1, simBoxInstanceBuffer);
        cmd.setIndexBuffer(simBoxIndexBuffer, rhi::IndexFormat::U16);
        cmd.drawIndexed(36, simBoxInstances);
    }
}

} // namespace render
