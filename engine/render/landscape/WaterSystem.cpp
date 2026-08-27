#include "engine/render/landscape/WaterSystem.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/Clock.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/terrain/RiverGeometry.hpp"
#include "engine/terrain/WaterInfoMap.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {

constexpr const char* kWaterShader = "water";
constexpr const char* kWaterLocalShader = "waterlocal";
constexpr const char* kWaterSimShader = "water_sim";

// Worker-side pool-depth bake: vertical water column over the terrain
// (sea + lakes + river ribbons when bodies are set), then a separable
// max-dilation (~36 m) so one texture tap answers "how deep does this
// POOL get nearby" — the small-pool foam criterion.
vector<f32> bakePoolDepth(const TerrainParams& params,
                          const sptr<const WaterBodies>& bodies,
                          Vec2 center) {
    constexpr u32 kSize = WaterSystem::kPoolMapSize;
    constexpr i32 kDilate = 3; // texels (~36 m radius)

    vector<f32> depth(static_cast<size_t>(kSize) * kSize);
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const f32 wx = center.x +
                           (static_cast<f32>(x) / kSize - 0.5f) *
                               WaterSystem::kPoolMapSpan;
            const f32 wz = center.y +
                           (static_cast<f32>(y) / kSize - 0.5f) *
                               WaterSystem::kPoolMapSpan;
            const f32 ground = terrain::height(params, wx, wz);
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

// Boundary inflow from the BAKED ribbons: any river polyline entering
// the window injects a discharge derived from its width (the inverse
// of the classifyRivers width law, default coef 0.008/exponent 0.5 —
// the ribbon's width says how much catchment feeds it). Master-network
// entries already inject the true areas; skip anything near one.
vector<render::terraingen::WaterSource> bakedEntrySources(
    const render::WaterBodies& bodies,
    const render::terraingen::GridSpec& spec,
    const vector<render::terraingen::WaterSource>& master,
    f32 rainRate) {
    using render::terraingen::WaterSource;
    render::terraingen::WaterSolveParams runoff;
    runoff.rainRate = rainRate;
    const f32 maxX = spec.originX +
                     static_cast<f32>(spec.n - 1) * spec.texelSize;
    const f32 maxZ = spec.originZ +
                     static_cast<f32>(spec.n - 1) * spec.texelSize;
    const auto inside = [&](f32 x, f32 z) {
        return x >= spec.originX && x <= maxX && z >= spec.originZ &&
               z <= maxZ;
    };
    vector<WaterSource> out;
    for (const render::RiverSurface& river : bodies.rivers) {
        for (size_t k = 1; k < river.nodes.size(); ++k) {
            const render::RiverNode& a = river.nodes[k - 1];
            const render::RiverNode& b = river.nodes[k];
            if (inside(a.x, a.z) || !inside(b.x, b.z)) {
                continue;
            }
            bool nearMaster = false;
            for (const WaterSource& m : master) {
                const f32 dx = m.x - b.x;
                const f32 dz = m.z - b.z;
                if (dx * dx + dz * dz < 80.0f * 80.0f) {
                    nearMaster = true;
                    break;
                }
            }
            if (nearMaster) {
                continue;
            }
            const f32 hw = glm::max(b.halfWidth, 0.5f);
            const f32 area = (hw / 0.008f) * (hw / 0.008f);
            out.push_back(
                { b.x, b.z,
                  glm::min(render::terraingen::runoffDischarge(area,
                                                               runoff),
                           120.0f) });
        }
    }
    return out;
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

void WaterSystem::create(rhi::Device& device, ShaderLibrary& shaders,
                         core::JobSystem& jobSystem) {
    jobs = &jobSystem;
    shared = std::make_shared<Shared>();

    vertexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex, .size = sizeof(kQuadVertices) },
        kQuadVertices);
    indexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Index, .size = sizeof(kQuadIndices) },
        kQuadIndices);

    poolMapSampler = device.createSampler({}); // linear clamp
    // 1x1 "deep everywhere" placeholder until the first bake lands: foam
    // behaves as before for a second.
    const f32 kDeep = 10.0f;
    poolMap = device.createTexture({ .width = 1,
                                     .height = 1,
                                     .format = rhi::TextureFormat::R16F,
                                     .usage = rhi::TextureUsage_Sampled },
                                   &kDeep);
    // Water-info placeholders: dry everywhere, zero flow — the validity
    // flag keeps the shader off them until a real bake lands anyway.
    const f32 kDry = terrain::kWaterInfoDry;
    infoMapA = device.createTexture({ .width = 1,
                                      .height = 1,
                                      .format = rhi::TextureFormat::R32F,
                                      .usage = rhi::TextureUsage_Sampled },
                                    &kDry);
    const f32 kZeros[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    infoMapB =
        device.createTexture({ .width = 1,
                               .height = 1,
                               .format = rhi::TextureFormat::RGBA16F,
                               .usage = rhi::TextureUsage_Sampled },
                             kZeros);
    // Sim placeholders: dry everywhere until the first pre-roll lands
    // (created before the first bind-group build below).
    const f32 kTuck = -1.0e6f;
    simMapA = device.createTexture({ .width = 1,
                                     .height = 1,
                                     .format = rhi::TextureFormat::R32F,
                                     .usage = rhi::TextureUsage_Sampled },
                                   &kTuck);
    simMapB =
        device.createTexture({ .width = 1,
                               .height = 1,
                               .format = rhi::TextureFormat::RGBA16F,
                               .usage = rhi::TextureUsage_Sampled },
                             kZeros);
    rebuildMaterials(device); // also builds the map bind group

    shaders.load(kWaterShader, { { "FrameUbo", 0 } },
                 { { "uSceneColor", 0 },
                   { "uSceneDepth", 1 },
                   { "uPoolDepth", 3 },
                   { "uSkyClouds", 4 },
                   { "uWaterInfoA", 5 },
                   { "uWaterInfoB", 6 },
                   { "uWaterSimA", 7 },
                   { "uWaterSimB", 8 } });
    shaders.load(kWaterLocalShader,
                 { { "FrameUbo", 0 }, { "WaterMaterialsUbo", 1 } },
                 { { "uSceneColor", 0 },
                   { "uSceneDepth", 1 },
                   { "uPoolDepth", 3 },
                   { "uSkyClouds", 4 },
                   { "uWaterInfoA", 5 },
                   { "uWaterInfoB", 6 },
                   { "uWaterSimA", 7 },
                   { "uWaterSimB", 8 } });
    shaders.load(kWaterSimShader,
                 { { "FrameUbo", 0 }, { "WaterMaterialsUbo", 1 } },
                 { { "uSceneColor", 0 },
                   { "uSceneDepth", 1 },
                   { "uPoolDepth", 3 },
                   { "uSkyClouds", 4 },
                   { "uWaterInfoA", 5 },
                   { "uWaterInfoB", 6 },
                   { "uWaterSimA", 7 },
                   { "uWaterSimB", 8 } });
    buildPipeline(device, shaders);
}

void WaterSystem::destroy(rhi::Device& device) {
    ++generation; // in-flight bakes die on arrival (shared queue outlives us)
    device.destroyBindGroup(poolMapGroup);
    device.destroyTexture(poolMap);
    device.destroyTexture(infoMapA);
    device.destroyTexture(infoMapB);
    device.destroyBuffer(materialsUbo);
    materialsUbo = {};
    infoMapA = {};
    infoMapB = {};
    infoValid = false;
    infoBakeInFlight = false;
    infoCenter = { 1.0e9f, 1.0e9f };
    infoBodiesStamp = ~0ull;
    device.destroySampler(poolMapSampler);
    device.destroyPipeline(pipeline);
    device.destroyBuffer(indexBuffer);
    device.destroyBuffer(vertexBuffer);
    device.destroyPipeline(localPipeline);
    device.destroyBuffer(localIndexBuffer);
    device.destroyBuffer(localVertexBuffer);
    poolMapGroup = {};
    poolMap = {};
    poolMapSampler = {};
    pipeline = {};
    indexBuffer = {};
    vertexBuffer = {};
    localPipeline = {};
    localIndexBuffer = {};
    localVertexBuffer = {};
    localIndexCount = 0;
    // Freshness state, so a destroy()+create() cycle re-bakes instead of
    // trusting a pool map that no longer exists.
    bakeInFlight = false;
    mapCenter = { 1e9f, 1e9f };
    bakedSeed = 0;
    bakedSeaLevel = -1e9f;
    bakedBodiesStamp = ~0ull;
    bodies.reset();
    bodiesStamp = 0;
    bodiesDirty = false;
    // Sim window teardown (in-flight jobs die on arrival by generation).
    device.destroyTexture(simMapA);
    device.destroyTexture(simMapB);
    device.destroyBuffer(simVertexBuffer);
    device.destroyBuffer(simIndexBuffer);
    device.destroyPipeline(simPipeline);
    simMapA = {};
    simMapB = {};
    simVertexBuffer = {};
    simIndexBuffer = {};
    simPipeline = {};
    simIndexCount = 0;
    simMeshN = 0;
    simState.reset();
    simSnap.reset();
    simSrcCache.clear();
    simInFlight = false;
    simGroundDirty = false;
    simValid = false;
    ++simEpoch;
    simAccum = 0.0f;
    simHasLastCam = false;
    simLastMs = 0.0f;
}

void WaterSystem::setBodies(sptr<const WaterBodies> next) {
    bodies = std::move(next);
    ++bodiesStamp;
    bodiesDirty = true;
}

void WaterSystem::rebuildLocalGeometry(rhi::Device& device) {
    device.destroyBuffer(localIndexBuffer);
    device.destroyBuffer(localVertexBuffer);
    localIndexBuffer = {};
    localVertexBuffer = {};
    localIndexCount = 0;
    if (!bodies || (bodies->lakes.empty() && bodies->rivers.empty())) {
        return;
    }
    // Layout: pos (3) + flow dir*speed (2) + {halfWidth, lateral (lake
    // quads: shore-foam gate), arcLength, endDist} (4) — the RIVER UV
    // SPACE the shared shading uses for flow mapping and the
    // end-of-course dissolve (the ribbon fades out INTO the pond/river
    // it merges with).
    constexpr u32 kFloatsPerVertex = 10;
    vector<f32> verts;
    vector<u32> indices;
    const auto vertex = [&](f32 x, f32 y, f32 z, f32 flowX, f32 flowZ,
                            f32 halfWidth, f32 lateral, f32 arc,
                            f32 endDist, f32 material) {
        verts.push_back(x);
        verts.push_back(y);
        verts.push_back(z);
        verts.push_back(flowX);
        verts.push_back(flowZ);
        verts.push_back(halfWidth);
        verts.push_back(lateral);
        verts.push_back(arc);
        verts.push_back(endDist);
        verts.push_back(material);
        return static_cast<u32>(verts.size() / kFloatsPerVertex - 1);
    };
    // `foamGate` rides the lateral lane (unused on lake quads): 1 =
    // shore foam allowed (big lakes), 0 = calm sheet (junction pools,
    // mountain tarns — the lapping ring read as noise on small water).
    const auto quad = [&](f32 x0, f32 z0, f32 x1, f32 z1, f32 level,
                          f32 material, f32 foamGate) {
        const u32 v0 = vertex(x0, level, z0, 0.0f, 0.0f, 0.0f, foamGate,
                              0.0f, 1.0e6f, material);
        const u32 v1 = vertex(x1, level, z0, 0.0f, 0.0f, 0.0f, foamGate,
                              0.0f, 1.0e6f, material);
        const u32 v2 = vertex(x1, level, z1, 0.0f, 0.0f, 0.0f, foamGate,
                              0.0f, 1.0e6f, material);
        const u32 v3 = vertex(x0, level, z1, 0.0f, 0.0f, 0.0f, foamGate,
                              0.0f, 1.0e6f, material);
        for (const u32 i : { v0, v2, v1, v0, v3, v2 }) {
            indices.push_back(i);
        }
    };
    // Shore foam only on lakes >= this wet area — below it the ring
    // dominates the surface instead of trimming it.
    constexpr f32 kFoamLakeArea = 5000.0f; // m²
    const auto materialOf = [&](u32 index) {
        return static_cast<f32>(
            glm::min(index, kMaxWaterMaterials - 1));
    };
    for (const LakeSurface& lake : bodies->lakes) {
        // The surface follows the basin mask (row runs of water cells,
        // grown half a texel so the sheet reaches under the banks — the
        // depth test cuts the exact shoreline). Maskless lakes
        // (hand-authored ponds) stay one bbox quad.
        if (lake.mask.empty() || lake.maskWidth == 0) {
            constexpr f32 kMargin = 6.0f;
            const f32 area = (lake.maxX - lake.minX) *
                             (lake.maxZ - lake.minZ);
            quad(lake.minX - kMargin, lake.minZ - kMargin,
                 lake.maxX + kMargin, lake.maxZ + kMargin, lake.level,
                 materialOf(lake.materialIndex),
                 area >= kFoamLakeArea ? 1.0f : 0.0f);
            continue;
        }
        u32 wet = 0;
        for (const u8 m : lake.mask) {
            wet += m ? 1 : 0;
        }
        const f32 foamGate = static_cast<f32>(wet) * lake.maskTexel *
                                     lake.maskTexel >=
                                 kFoamLakeArea
                                 ? 1.0f
                                 : 0.0f;
        const f32 grow = lake.maskTexel * 0.75f;
        for (u32 row = 0; row < lake.maskHeight; ++row) {
            const f32 z0 =
                lake.minZ + static_cast<f32>(row) * lake.maskTexel - grow;
            const f32 z1 = z0 + lake.maskTexel + 2.0f * grow;
            u32 col = 0;
            while (col < lake.maskWidth) {
                if (!lake.mask[static_cast<size_t>(row) * lake.maskWidth +
                               col]) {
                    ++col;
                    continue;
                }
                u32 end = col;
                while (end + 1 < lake.maskWidth &&
                       lake.mask[static_cast<size_t>(row) *
                                     lake.maskWidth +
                                 end + 1]) {
                    ++end;
                }
                quad(lake.minX + static_cast<f32>(col) * lake.maskTexel -
                         grow,
                     z0,
                     lake.minX + static_cast<f32>(end) * lake.maskTexel +
                         lake.maskTexel + grow,
                     z1, lake.level, materialOf(lake.materialIndex),
                     foamGate);
                col = end + 1;
            }
        }
    }
    for (const RiverSurface& river : bodies->rivers) {
        // Ribbon strip along the polyline at the water surface, slightly
        // wider than the carved bed so banks clip it. Tight bends are
        // subdivided by curvature (RiverGeometry) — the water-info
        // raster samples the same conditioned curve.
        const vector<RiverNode> nodes =
            terrain::subdivideRiverNodes(river.nodes);
        u32 prevL = 0;
        u32 prevR = 0;
        f32 totalArc = 0.0f;
        for (size_t i = 1; i < nodes.size(); ++i) {
            totalArc += std::hypot(nodes[i].x - nodes[i - 1].x,
                                   nodes[i].z - nodes[i - 1].z);
        }
        f32 arc = 0.0f;
        for (size_t i = 0; i < nodes.size(); ++i) {
            const RiverNode& node = nodes[i];
            const RiverNode& ahead =
                nodes[glm::min(i + 1, nodes.size() - 1)];
            const RiverNode& behind = nodes[i > 0 ? i - 1 : 0];
            if (i > 0) {
                arc += std::hypot(node.x - nodes[i - 1].x,
                                  node.z - nodes[i - 1].z);
            }
            f32 dx = ahead.x - behind.x;
            f32 dz = ahead.z - behind.z;
            const f32 len = std::sqrt(dx * dx + dz * dz);
            if (len > 1.0e-3f) {
                dx /= len;
                dz /= len;
            } else {
                dx = 1.0f;
                dz = 0.0f;
            }
            const f32 half = node.halfWidth * 1.15f;
            const f32 flowX = dx * river.flowSpeed;
            const f32 flowZ = dz * river.flowSpeed;
            const u32 left =
                vertex(node.x - dz * half, node.surface,
                       node.z + dx * half, flowX, flowZ, node.halfWidth,
                       -1.0f, arc, totalArc - arc,
                       materialOf(river.materialIndex));
            const u32 right =
                vertex(node.x + dz * half, node.surface,
                       node.z - dx * half, flowX, flowZ, node.halfWidth,
                       1.0f, arc, totalArc - arc,
                       materialOf(river.materialIndex));
            if (i > 0) {
                for (const u32 v : { prevL, prevR, left, prevR, right,
                                     left }) {
                    indices.push_back(v);
                }
            }
            prevL = left;
            prevR = right;
        }
    }
    if (verts.empty() || indices.empty()) {
        return;
    }
    localVertexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = verts.size() * sizeof(f32) },
        verts.data());
    localIndexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = indices.size() * sizeof(u32) },
        indices.data());
    localIndexCount = static_cast<u32>(indices.size());
}

void WaterSystem::update(rhi::Device& device, const TerrainParams& params,
                         const Vec3& cameraPos) {
    // Apply a finished bake: new texture + bind group (rebakes are rare —
    // every ~500 m of travel or on a settings change).
    BakedMap baked;
    while (shared->baked.tryPop(baked)) {
        if (baked.generation != generation) {
            continue;
        }
        device.destroyTexture(poolMap);
        poolMap = device.createTexture(
            { .width = kPoolMapSize,
              .height = kPoolMapSize,
              .format = rhi::TextureFormat::R16F,
              .filter = rhi::FilterMode::Linear,
              .usage = rhi::TextureUsage_Sampled },
            baked.texels.data());
        rebuildMapGroup(device);
        mapCenter = baked.center;
        bakedSeed = baked.seed;
        bakedSeaLevel = baked.seaLevel;
        bakedBodiesStamp = baked.bodiesStamp;
        bakeInFlight = false;
    }
    BakedInfo info;
    while (shared->bakedInfo.tryPop(info)) {
        if (info.generation != generation) {
            continue;
        }
        device.destroyTexture(infoMapA);
        device.destroyTexture(infoMapB);
        infoMapA = device.createTexture(
            { .width = kInfoMapSize,
              .height = kInfoMapSize,
              .format = rhi::TextureFormat::R32F,
              .filter = rhi::FilterMode::Linear,
              .usage = rhi::TextureUsage_Sampled },
            info.surface.data());
        infoMapB = device.createTexture(
            { .width = kInfoMapSize,
              .height = kInfoMapSize,
              .format = rhi::TextureFormat::RGBA16F,
              .filter = rhi::FilterMode::Linear,
              .usage = rhi::TextureUsage_Sampled },
            info.extras.data());
        rebuildMapGroup(device);
        infoCenter = info.center;
        infoSeed = info.seed;
        infoBodiesStamp = info.bodiesStamp;
        infoValid = true;
        infoBakeInFlight = false;
    }

    if (bodiesDirty) {
        rebuildLocalGeometry(device);
        rebuildMaterials(device);
        bodiesDirty = false;
    }

    const Vec2 camXz { cameraPos.x, cameraPos.z };
    const bool stale = glm::distance(camXz, mapCenter) > kRebakeDistance ||
                       bakedSeed != params.seed ||
                       bakedSeaLevel != params.seaLevel ||
                       bakedBodiesStamp !=
                           bodiesStamp + params.contentStamp;
    if (stale && !bakeInFlight) {
        bakeInFlight = true;
        constexpr f32 kTexel = kPoolMapSpan / kPoolMapSize;
        const Vec2 center = glm::floor(camXz / kTexel) * kTexel;
        jobs->enqueue([sharedRef = shared, params, center,
                       gen = generation, bodiesRef = bodies,
                       stamp = bodiesStamp + params.contentStamp] {
            sharedRef->baked.push({ center, gen, params.seed,
                                    params.seaLevel, stamp,
                                    bakePoolDepth(params, bodiesRef,
                                                  center) });
        });
    }

    // Water-info map: same predicate shape, its own (tighter) follow
    // distance. On a CONTENT change the old texture must not resolve
    // junctions against dead bodies — drop the validity flag until the
    // fresh bake lands (the shader then uses pure vertex data, i.e.
    // exactly the pre-info rendering).
    const u64 wantedStamp = bodiesStamp + params.contentStamp;
    const bool infoStale =
        glm::distance(camXz, infoCenter) > kInfoRebakeDistance ||
        infoSeed != params.seed || infoBodiesStamp != wantedStamp;
    if (infoBodiesStamp != wantedStamp) {
        infoValid = false;
    }
    if (infoStale && !infoBakeInFlight) {
        infoBakeInFlight = true;
        jobs->enqueue([sharedRef = shared, params, camXz,
                       gen = generation, bodiesRef = bodies,
                       stamp = wantedStamp] {
            BakedInfo out;
            out.generation = gen;
            out.seed = params.seed;
            out.bodiesStamp = stamp;
            const WaterBodies empty;
            terrain::WaterInfoMap map = terrain::bakeWaterInfo(
                bodiesRef ? *bodiesRef : empty, camXz, kInfoMapSpan,
                kInfoMapSize, [&params](f32 x, f32 z) {
                    return terrain::height(params, x, z);
                });
            out.center = map.center;
            out.surface = std::move(map.surface);
            out.extras.resize(map.depth.size() * 4);
            for (size_t i = 0; i < map.depth.size(); ++i) {
                out.extras[i * 4 + 0] = map.depth[i];
                out.extras[i * 4 + 1] = map.flow[i].x;
                out.extras[i * 4 + 2] = map.flow[i].y;
                out.extras[i * 4 + 3] = 0.0f;
            }
            sharedRef->bakedInfo.push(std::move(out));
        });
    }
}

Vec4 WaterSystem::simMapInfo() const {
    if (!simValid || !simSnap || simSnap->spec.n < 2) {
        return { 0.0f, 0.0f, 0.0f, 0.0f };
    }
    const auto& spec = simSnap->spec;
    const f32 span = static_cast<f32>(spec.n - 1) * spec.texelSize;
    return { spec.originX, spec.originZ, 1.0f / span, 1.0f };
}

void WaterSystem::uploadSimTextures(rhi::Device& device,
                                    const terrain::WaterSimSnapshot& snap) {
    // Destroy+create each tick (the proven info-map path; both
    // backends defer deletion). An updateTexture RHI fast path is a
    // known later optimization.
    const u32 n = snap.spec.n;
    device.destroyTexture(simMapA);
    device.destroyTexture(simMapB);
    simMapA = device.createTexture(
        { .width = n,
          .height = n,
          .format = rhi::TextureFormat::R32F,
          .filter = rhi::FilterMode::Linear,
          .usage = rhi::TextureUsage_Sampled },
        snap.display.data());
    vector<f32> extras(static_cast<size_t>(n) * n * 4);
    for (size_t i = 0; i < snap.depth.size(); ++i) {
        extras[i * 4 + 0] = snap.depth[i];
        extras[i * 4 + 1] = snap.velX[i];
        extras[i * 4 + 2] = snap.velZ[i];
        extras[i * 4 + 3] = 0.0f;
    }
    simMapB = device.createTexture(
        { .width = n,
          .height = n,
          .format = rhi::TextureFormat::RGBA16F,
          .filter = rhi::FilterMode::Linear,
          .usage = rhi::TextureUsage_Sampled },
        extras.data());
    rebuildMapGroup(device);
    ensureSimMesh(device, n);
}

void WaterSystem::ensureSimMesh(rhi::Device& device, u32 n) {
    if (simMeshN == n || n < 2) {
        return;
    }
    device.destroyBuffer(simVertexBuffer);
    device.destroyBuffer(simIndexBuffer);
    // Node-uv grid; water_sim.vert lifts each node by the display
    // texture. Winding matches the sea quad (CCW from above).
    vector<f32> verts;
    verts.reserve(static_cast<size_t>(n) * n * 2);
    const f32 inv = 1.0f / static_cast<f32>(n - 1);
    for (u32 row = 0; row < n; ++row) {
        for (u32 col = 0; col < n; ++col) {
            verts.push_back(static_cast<f32>(col) * inv);
            verts.push_back(static_cast<f32>(row) * inv);
        }
    }
    vector<u32> indices;
    indices.reserve(static_cast<size_t>(n - 1) * (n - 1) * 6);
    for (u32 row = 0; row + 1 < n; ++row) {
        for (u32 col = 0; col + 1 < n; ++col) {
            const u32 v00 = row * n + col;
            const u32 v10 = v00 + 1;
            const u32 v01 = v00 + n;
            const u32 v11 = v01 + 1;
            for (const u32 v : { v00, v11, v10, v00, v01, v11 }) {
                indices.push_back(v);
            }
        }
    }
    simVertexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = verts.size() * sizeof(f32) },
        verts.data());
    simIndexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = indices.size() * sizeof(u32) },
        indices.data());
    simIndexCount = static_cast<u32>(indices.size());
    simMeshN = n;
}

void WaterSystem::updateSim(rhi::Device& device,
                            const TerrainParams& params,
                            const Vec3& cameraPos, f32 dt) {
    using terrain::WaterSimState;
    // Apply finished jobs (newest wins; at most one is ever in flight).
    SimResult res;
    while (shared->simDone.tryPop(res)) {
        if (res.generation != generation) {
            continue;
        }
        simInFlight = false;
        if (res.epoch != simEpoch) {
            continue; // invalidated while the job ran
        }
        simState = res.state;
        simLastMs = res.millis;
        if (res.sourcesFresh) {
            simSrcCache = std::move(res.sources);
        }
        if (res.snap) {
            uploadSimTextures(device, *res.snap);
            simSnap = res.snap;
            simValid = true;
        }
    }
    if (!simCfg.enabled || !params.base) {
        if (simState || simValid) {
            simState.reset();
            simSnap.reset();
            simValid = false;
            ++simEpoch;
            simAccum = 0.0f;
        }
        return;
    }
    // Sustained fast travel (fly mode): no margin survives 25+ m/s —
    // drop the window, the baked bodies take over, and slowing down
    // re-enters like a teleport (fresh pre-roll).
    const Vec2 camXz { cameraPos.x, cameraPos.z };
    f32 speed = 0.0f;
    if (simHasLastCam && dt > 1.0e-4f) {
        speed = glm::distance(camXz, simLastCam) / dt;
    }
    simLastCam = camXz;
    simHasLastCam = true;
    if (speed > simCfg.invalidateSpeed) {
        if (simState || simValid) {
            simState.reset();
            simSnap.reset();
            simValid = false;
            ++simEpoch;
            simAccum = 0.0f;
        }
        return;
    }
    if (simInFlight) {
        return;
    }
    const f32 texel = glm::max(simCfg.texel, 0.5f);
    // Live span/texel knob change: rebuild the window from scratch.
    if (simState &&
        (std::abs(simState->spec.texelSize - texel) > 1.0e-3f ||
         std::abs(static_cast<f32>(simState->spec.n - 1) *
                      simState->spec.texelSize -
                  simCfg.span) > texel * 2.0f)) {
        simState.reset();
        simSnap.reset();
        simValid = false;
        ++simEpoch;
        simAccum = 0.0f;
    }
    if (!simState) {
        // Wait for the ground truth: pre-rolling before the camera's
        // tile is published would settle water on the analytic
        // fallback terrain, meters off the baked one.
        if (!params.base->regionAt(cameraPos.x, cameraPos.z)) {
            return;
        }
        // Teleport / first entry: async pre-roll to equilibrium.
        terraingen::GridSpec spec;
        spec.texelSize = texel;
        spec.n = static_cast<u32>(simCfg.span / texel) + 1;
        spec.originX =
            std::floor((cameraPos.x - simCfg.span * 0.5f) / texel) *
            texel;
        spec.originZ =
            std::floor((cameraPos.z - simCfg.span * 0.5f) / texel) *
            texel;
        simInFlight = true;
        const f32 maxX =
            spec.originX + static_cast<f32>(spec.n - 1) * texel;
        const f32 maxZ =
            spec.originZ + static_cast<f32>(spec.n - 1) * texel;
        jobs->enqueue([sharedRef = shared, params, spec,
                       simParams = simCfg.params, fn = simSourcesFn,
                       bodiesRef = bodies, gen = generation,
                       epoch = simEpoch, maxX, maxZ] {
            SimResult out;
            out.generation = gen;
            out.epoch = epoch;
            const core::TimePoint start = core::clockNow();
            if (fn) {
                out.sources = fn(spec.originX, spec.originZ, maxX, maxZ);
            }
            if (bodiesRef) {
                const auto baked = bakedEntrySources(
                    *bodiesRef, spec, out.sources, simParams.rainRate);
                out.sources.insert(out.sources.end(), baked.begin(),
                                   baked.end());
            }
            out.sourcesFresh = true;
            auto state = std::make_shared<WaterSimState>(
                terrain::preRollWindow(
                    spec,
                    [&params](f32 x, f32 z) {
                        return terrain::height(params, x, z);
                    },
                    simParams, out.sources));
            if (bodiesRef) {
                // Baked lakes = pinned reservoirs; one substep applies
                // them so the first snapshot already holds the lakes.
                terrain::pinLakes(*state, *bodiesRef);
                terrain::stepWindow(*state, simParams, out.sources, 1);
            }
            auto snap = std::make_shared<terrain::WaterSimSnapshot>();
            terrain::extractSnapshot(*state, simParams, *snap);
            out.state = std::move(state);
            out.snap = std::move(snap);
            out.millis =
                static_cast<f32>(core::secondsSince(start) * 1000.0);
            sharedRef->simDone.push(std::move(out));
        });
        return;
    }
    // Regular tick: fixed-dt accumulator (hitch-clamped), re-anchor by
    // hysteresis, ground refresh on terraforming.
    simAccum += glm::min(dt, 0.25f);
    const f32 tickDt = glm::max(simCfg.params.dt, 1.0f / 240.0f);
    u32 substeps = static_cast<u32>(simAccum / tickDt);
    substeps = glm::min(substeps, simCfg.maxSubsteps);
    simAccum = glm::min(simAccum - static_cast<f32>(substeps) * tickDt,
                        tickDt);
    const auto& spec = simState->spec;
    const f32 span = static_cast<f32>(spec.n - 1) * spec.texelSize;
    const f32 centerX = spec.originX + span * 0.5f;
    const f32 centerZ = spec.originZ + span * 0.5f;
    i32 dCol = 0;
    i32 dRow = 0;
    if (std::abs(cameraPos.x - centerX) > simCfg.anchorHysteresis) {
        dCol = static_cast<i32>(
            std::lround((cameraPos.x - centerX) / spec.texelSize));
    }
    if (std::abs(cameraPos.z - centerZ) > simCfg.anchorHysteresis) {
        dRow = static_cast<i32>(
            std::lround((cameraPos.z - centerZ) / spec.texelSize));
    }
    if (substeps == 0 && dCol == 0 && dRow == 0 && !simGroundDirty) {
        return;
    }
    simInFlight = true;
    const bool refreshGround = simGroundDirty;
    simGroundDirty = false;
    jobs->enqueue([sharedRef = shared, params, state = simState,
                   simParams = simCfg.params, fn = simSourcesFn,
                   bodiesRef = bodies, sources = simSrcCache,
                   gen = generation, epoch = simEpoch, dCol, dRow,
                   substeps, refreshGround]() mutable {
        SimResult out;
        out.generation = gen;
        out.epoch = epoch;
        const core::TimePoint start = core::clockNow();
        const auto heightFn = [&params](f32 x, f32 z) {
            return terrain::height(params, x, z);
        };
        if (refreshGround) {
            terrain::refreshTerrain(*state, heightFn);
        }
        if (dCol != 0 || dRow != 0) {
            terrain::scrollWindow(*state, dCol, dRow, heightFn,
                                  simParams.seaLevel);
            const auto& sp = state->spec;
            const f32 sMaxX =
                sp.originX + static_cast<f32>(sp.n - 1) * sp.texelSize;
            const f32 sMaxZ =
                sp.originZ + static_cast<f32>(sp.n - 1) * sp.texelSize;
            sources.clear();
            if (fn) {
                sources = fn(sp.originX, sp.originZ, sMaxX, sMaxZ);
            }
            if (bodiesRef) {
                const auto baked = bakedEntrySources(
                    *bodiesRef, sp, sources, simParams.rainRate);
                sources.insert(sources.end(), baked.begin(),
                               baked.end());
            }
            out.sourcesFresh = true;
        }
        if (bodiesRef && (refreshGround || dCol != 0 || dRow != 0)) {
            // Ground or window moved: re-rasterize the reservoirs (and
            // apply them — at least one substep).
            terrain::pinLakes(*state, *bodiesRef);
            substeps = glm::max(substeps, 1u);
        }
        terrain::stepWindow(*state, simParams, sources, substeps);
        auto snap = std::make_shared<terrain::WaterSimSnapshot>();
        terrain::extractSnapshot(*state, simParams, *snap);
        out.state = std::move(state);
        out.snap = std::move(snap);
        out.sources = std::move(sources);
        out.millis =
            static_cast<f32>(core::secondsSince(start) * 1000.0);
        sharedRef->simDone.push(std::move(out));
    });
}

void WaterSystem::rebuildMapGroup(rhi::Device& device) {
    device.destroyBindGroup(poolMapGroup);
    poolMapGroup = device.createBindGroup(
        { .entries = { { .binding = 1, .buffer = materialsUbo },
                       { .binding = 3,
                         .texture = poolMap,
                         .sampler = poolMapSampler },
                       { .binding = 5,
                         .texture = infoMapA,
                         .sampler = poolMapSampler },
                       { .binding = 6,
                         .texture = infoMapB,
                         .sampler = poolMapSampler },
                       { .binding = 7,
                         .texture = simMapA,
                         .sampler = poolMapSampler },
                       { .binding = 8,
                         .texture = simMapB,
                         .sampler = poolMapSampler } } });
}

void WaterSystem::rebuildMaterials(rhi::Device& device) {
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
    device.destroyBuffer(materialsUbo);
    materialsUbo = device.createBuffer(
        { .usage = rhi::BufferUsage::Uniform,
          .size = table.size() * sizeof(GpuWaterMaterial) },
        table.data());
    rebuildMapGroup(device);
}

void WaterSystem::buildPipeline(rhi::Device& device, ShaderLibrary& shaders) {
    if (pipeline.id != 0) {
    shaders.beginWatch();
        device.destroyPipeline(pipeline);
    }
    pipeline = device.createPipeline(
        { .shader = shaders.get(kWaterShader),
          .vertexBuffers =
              { { .stride = 2 * sizeof(f32),
                  .attributes = { { .location = 0,
                                    .format = rhi::VertexFormat::F32x2,
                                    .offset = 0 } } } },
          // Opaque (refraction is composed manually from the scene
          // snapshot); depth-tested against the opaque pass, and written so
          // fog-of-depth effects later stay consistent. Two-sided: the
          // shader renders a distinct underside when seen from below.
          // Toward-camera bias (positive under reversed-Z + Greater):
          // where the sheet lies centimetres over the ground (shallow
          // shelves, flooded meadow dips) the two planes can sit inside
          // the depth-quantization noise and the surface flickered pixel
          // by pixel — contour-line moiré fringes. The bias rides the
          // depth format's local precision, so it stays microscopic
          // everywhere reversed-Z keeps precision healthy.
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Greater }, // reversed-Z
          .cull = rhi::CullMode::None,
          .depthBias = 4.0f,
          .depthBiasSlope = 2.5f });
    // Local surfaces: same states, world-space F32x3 vertices.
    if (localPipeline.id != 0) {
        device.destroyPipeline(localPipeline);
    }
    localPipeline = device.createPipeline(
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
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Greater },
          .cull = rhi::CullMode::None,
          .depthBias = 4.0f,
          .depthBiasSlope = 2.5f });
    // Sim window: node-uv grid displaced by the display texture in the
    // vertex shader, same states and shared shading.
    if (simPipeline.id != 0) {
        device.destroyPipeline(simPipeline);
    }
    simPipeline = device.createPipeline(
        { .shader = shaders.get(kWaterSimShader),
          .vertexBuffers =
              { { .stride = 2 * sizeof(f32),
                  .attributes = { { .location = 0,
                                    .format = rhi::VertexFormat::F32x2,
                                    .offset = 0 } } } },
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Greater },
          .cull = rhi::CullMode::None,
          .depthBias = 4.0f,
          .depthBiasSlope = 2.5f });
    // The watch recorded every shader the build consumed: a reload of
    // any of them rebuilds the pipelines.
    shaderWatch = shaders.endWatch();
}

void WaterSystem::refreshPipeline(rhi::Device& device,
                                  ShaderLibrary& shaders) {
    if (shaderWatch.changed(shaders)) {
        buildPipeline(device, shaders);
    }
}

void WaterSystem::draw(rhi::CommandBuffer& cmd,
                       rhi::BindGroupHandle frameBindGroup,
                       rhi::BindGroupHandle sceneBindGroup) {
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
    if (simValid && simIndexCount > 0 && simCfg.debugMode != 1) {
        // The live sim sheet (drawn last: its fragments discard where
        // dry, the baked ones discard where the sim owns the texel).
        cmd.setPipeline(simPipeline);
        cmd.setBindGroup(0, frameBindGroup);
        cmd.setBindGroup(1, sceneBindGroup);
        cmd.setBindGroup(2, poolMapGroup);
        cmd.setVertexBuffer(0, simVertexBuffer);
        cmd.setIndexBuffer(simIndexBuffer, rhi::IndexFormat::U32);
        cmd.drawIndexed(simIndexCount);
    }
}

} // namespace render
