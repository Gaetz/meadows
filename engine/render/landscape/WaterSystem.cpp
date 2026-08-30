#include "engine/render/landscape/WaterSystem.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/Clock.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
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
constexpr const char* kWaterSimFrozenShader = "water_simfrozen";
constexpr const char* kWaterSimBoxShader = "water_simdbg";

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
    f32 rainRate, f32 pinnedHalfWidth) {
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
    const auto lawQ = [&](f32 halfWidth) {
        const f32 hw = glm::max(halfWidth, 0.0f);
        const f32 area = (hw / 0.008f) * (hw / 0.008f);
        return render::terraingen::runoffDischarge(area, runoff);
    };
    vector<WaterSource> out;
    for (const render::RiverSurface& river : bodies.rivers) {
        // DISTRIBUTED runoff along in-window reaches: the baked
        // half-width GROWS along a course as its local basin feeds it
        // — inject that growth (the width-law discharge delta between
        // consecutive nodes, accumulated to a threshold). Without it,
        // a course whose HEAD lies inside the window had no supply at
        // all (no boundary crossing, sub-threshold for pins): its
        // pre-roll water drained and the channel stayed dry (measured
        // dev, the tier-0 stream at (9534, 2819)). Pinned-wide
        // reaches skip it (the reservoir supplies).
        // hand-tuned: back to the raw law — the x2 boost predated the
        // v67 carve fix (channels exist everywhere now) and flooded
        // the plains once the water actually fit its beds (dev).
        constexpr f32 kRunoffBoost = 1.0f;
        f32 accum = 0.0f;
        for (size_t k = 1; k < river.nodes.size(); ++k) {
            const render::RiverNode& na = river.nodes[k - 1];
            const render::RiverNode& nb = river.nodes[k];
            if (inside(na.x, na.z) && inside(nb.x, nb.z) &&
                nb.halfWidth < pinnedHalfWidth) {
                accum += kRunoffBoost *
                         glm::max(0.0f, lawQ(nb.halfWidth) -
                                            lawQ(na.halfWidth));
                if (accum >= 0.05f) {
                    out.push_back(
                        { nb.x, nb.z, glm::min(accum, 30.0f) });
                    accum = 0.0f;
                }
            }
        }
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
            // A ribbon wide enough to be PINNED (E1b) supplies itself
            // through its reservoir cells — injecting an entry source
            // on top would double its discharge.
            if (b.halfWidth >= pinnedHalfWidth) {
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
    // Unit cube for the debug volume boxes (8 verts, 12 tris).
    constexpr f32 kCubeVerts[] = {
        0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0,
        0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1,
    };
    constexpr u16 kCubeIndices[] = {
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
        3, 6, 2, 3, 7, 6, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5,
    };
    simBoxVertexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex, .size = sizeof(kCubeVerts) },
        kCubeVerts);
    simBoxIndexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = sizeof(kCubeIndices) },
        kCubeIndices);
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
    shaders.load(kWaterSimFrozenShader,
                 { { "FrameUbo", 0 }, { "WaterMaterialsUbo", 1 } },
                 { { "uSceneColor", 0 },
                   { "uSceneDepth", 1 },
                   { "uPoolDepth", 3 },
                   { "uSkyClouds", 4 },
                   { "uWaterInfoA", 5 },
                   { "uWaterInfoB", 6 },
                   { "uWaterSimA", 7 },
                   { "uWaterSimB", 8 } });
    shaders.load(kWaterSimBoxShader, { { "FrameUbo", 0 } }, {});
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
    device.destroyPipeline(simPipelineOverlay);
    device.destroyPipeline(simFrozenPipeline);
    simFrozenPipeline = {};
    simFrozenClearNow(device);
    device.destroyPipeline(simBoxPipeline);
    device.destroyBuffer(simBoxVertexBuffer);
    device.destroyBuffer(simBoxIndexBuffer);
    device.destroyBuffer(simBoxInstanceBuffer);
    simMapA = {};
    simMapB = {};
    simVertexBuffer = {};
    simIndexBuffer = {};
    simPipeline = {};
    simPipelineOverlay = {};
    simBoxPipeline = {};
    simBoxVertexBuffer = {};
    simBoxIndexBuffer = {};
    simBoxInstanceBuffer = {};
    simBoxInstances = 0;
    simWetCells = 0;
    simIndexCount = 0;
    simState.reset();
    simSnap.reset();
    simSrcCache.clear();
    simCache.clear();
    simSettling = false;
    simHasLastCrumb = false;
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
    // Do NOT clear the crumb cache or the frozen meshes here: bodies
    // change on EVERY streamed tile, and wiping wiped the water
    // memory every few seconds of travel (dev: "the saved water and
    // its transitions are gone" — they only ever worked inside fully
    // streamed zones). Staleness is self-healing: pinLakes
    // re-rasterizes the pins from the CURRENT bodies on every resume
    // and scroll, and the frozen footprint refreshes on its timer.
    // The sculpt hook (a real ground change) keeps the full clear.
}

// Worker-side geometry build (see LocalMesh in the header): a pure
// function of the published bodies + terrain params — terrain::height
// is worker-proven (the sim jobs sample it against the same immutable
// snapshots).
void WaterSystem::buildLocalGeometry(const render::WaterBodies& bodiesIn,
                                     const render::TerrainParams& params,
                                     vector<f32>& verts,
                                     vector<u32>& indices) {
    const render::WaterBodies* bodies = &bodiesIn;
    // Layout: pos (3) + flow dir*speed (2) + {halfWidth, lateral (lake
    // quads: shore-foam gate), arcLength, endDist} (4) — the RIVER UV
    // SPACE the shared shading uses for flow mapping and the
    // end-of-course dissolve (the ribbon fades out INTO the pond/river
    // it merges with).
    constexpr u32 kFloatsPerVertex = 10;
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
        // One quad per covered texel, with PER-SIDE extension:
        // covered neighbour -> flush (adjacent quads share the edge
        // exactly); uncovered RISING bank -> grown 0.75 texel so the
        // sheet reaches under the bank and the depth test cuts the
        // exact shoreline (the historical behavior); uncovered
        // FALLING ground (below the level: an eroded crest) -> flush.
        // The old whole-run growth pushed the sheet 6 m into thin air
        // past every outlet crest — the jutting corner floating over
        // the void at the spawn lake (measured dev). Terrain is
        // sampled only on uncovered sides (the perimeter), so the
        // rebuild stays cheap.
        const f32 grow = lake.maskTexel * 0.75f;
        const f32 mt = lake.maskTexel;
        const auto covered = [&](i32 mc, i32 mr) {
            return mc >= 0 && mr >= 0 &&
                   mc < static_cast<i32>(lake.maskWidth) &&
                   mr < static_cast<i32>(lake.maskHeight) &&
                   lake.mask[static_cast<size_t>(mr) * lake.maskWidth +
                             static_cast<size_t>(mc)] != 0;
        };
        for (u32 row = 0; row < lake.maskHeight; ++row) {
            for (u32 col = 0; col < lake.maskWidth; ++col) {
                if (!covered(static_cast<i32>(col),
                             static_cast<i32>(row))) {
                    continue;
                }
                const f32 tx0 =
                    lake.minX + static_cast<f32>(col) * mt;
                const f32 tz0 =
                    lake.minZ + static_cast<f32>(row) * mt;
                const f32 cx = tx0 + 0.5f * mt;
                const f32 cz = tz0 + 0.5f * mt;
                // Per-side extension AND crest flag: a crest side (the
                // void past the edge) ends flush and then POURS — a
                // short outward-dipping skirt replaces the square slab
                // silhouette with an edge that starts its fall (the
                // "flan" softening, step 3a of the shores plan).
                struct SideExt {
                    f32 grow { 0.0f };
                    bool crest { false };
                };
                const auto ext = [&](i32 dc, i32 dr, f32 ex, f32 ez) {
                    SideExt se;
                    if (covered(static_cast<i32>(col) + dc,
                                static_cast<i32>(row) + dr)) {
                        return se;
                    }
                    // Probe a FULL mask texel past the edge (three
                    // samples), not just half a texel: at a mountain
                    // COL the near sample lands on the rising saddle
                    // while the ground plunges right behind — the
                    // grown sheet jutted square over the drop
                    // (measured dev). The extension is granted only
                    // if the ground HOLDS the level along the whole
                    // probe (a true bank, cut by the depth test); one
                    // deep, sibling-uncovered point = the void past a
                    // crest — flush, the water stops at its fall. A
                    // deep point covered by a SIBLING piece of the
                    // same lake is a tile seam: the grown overlap
                    // bridges the raster misalignment as before.
                    for (i32 s = 0; s < 3; ++s) {
                        const f32 px =
                            ex + static_cast<f32>(dc) *
                                     (0.5f + static_cast<f32>(s)) * mt;
                        const f32 pz =
                            ez + static_cast<f32>(dr) *
                                     (0.5f + static_cast<f32>(s)) * mt;
                        if (terrain::height(params, px, pz) >=
                            lake.level - 2.0f) {
                            continue;
                        }
                        bool seam = false;
                        for (const LakeSurface& other : bodies->lakes) {
                            if (&other == &lake) {
                                continue;
                            }
                            if (glm::abs(other.level - lake.level) <
                                    1.0f &&
                                other.covers(px, pz)) {
                                seam = true;
                                break;
                            }
                        }
                        if (!seam) {
                            se.crest = true;
                            return se;
                        }
                    }
                    se.grow = grow;
                    return se;
                };
                const SideExt west = ext(-1, 0, tx0, cz);
                const SideExt east = ext(1, 0, tx0 + mt, cz);
                const SideExt north = ext(0, -1, cx, tz0);
                const SideExt south = ext(0, 1, cx, tz0 + mt);
                const f32 x0 = tx0 - west.grow;
                const f32 z0 = tz0 - north.grow;
                const f32 x1 = tx0 + mt + east.grow;
                const f32 z1 = tz0 + mt + south.grow;
                // Step 3b: where TWO crest sides meet, the square
                // corner is CHAMFERED at 45° (half a texel per side)
                // — the horizontal silhouette softening, mirroring
                // what marching squares did for the sim shore. Crest
                // sides have grow = 0, so the cuts live on the raw
                // texel edges; bank/seam corners stay square (the
                // depth test owns those).
                const f32 ck = 0.5f * mt;
                const bool cutNW = west.crest && north.crest;
                const bool cutNE = east.crest && north.crest;
                const bool cutSE = east.crest && south.crest;
                const bool cutSW = west.crest && south.crest;
                f32 pxs[8];
                f32 pzs[8];
                u32 pcount = 0;
                const auto pt = [&](f32 px, f32 pz) {
                    if (pcount > 0 &&
                        std::abs(pxs[pcount - 1] - px) < 1.0e-4f &&
                        std::abs(pzs[pcount - 1] - pz) < 1.0e-4f) {
                        return;
                    }
                    pxs[pcount] = px;
                    pzs[pcount] = pz;
                    ++pcount;
                };
                if (cutNW) {
                    pt(x0 + ck, z0);
                } else {
                    pt(x0, z0);
                }
                if (cutNE) {
                    pt(x1 - ck, z0);
                    pt(x1, z0 + ck);
                } else {
                    pt(x1, z0);
                }
                if (cutSE) {
                    pt(x1, z1 - ck);
                    pt(x1 - ck, z1);
                } else {
                    pt(x1, z1);
                }
                if (cutSW) {
                    pt(x0 + ck, z1);
                    pt(x0, z1 - ck);
                }  else {
                    pt(x0, z1);
                }
                if (cutNW) {
                    pt(x0, z0 + ck);
                }
                const f32 mat = materialOf(lake.materialIndex);
                u32 vids[8];
                for (u32 q = 0; q < pcount; ++q) {
                    vids[q] = vertex(pxs[q], lake.level, pzs[q], 0.0f,
                                     0.0f, 0.0f, foamGate, 0.0f,
                                     1.0e6f, mat);
                }
                for (u32 q = 1; q + 1 < pcount; ++q) {
                    indices.push_back(vids[0]);
                    indices.push_back(vids[q + 1]);
                    indices.push_back(vids[q]);
                }
                // Crest skirts (step 3a): the pouring edge — a short
                // quad dipping OUTWARD from every crest segment
                // (shortened sides AND chamfer diagonals), so the
                // slab silhouette reads as water starting its fall.
                constexpr f32 kSkirtOut = 2.0f;  // m outward
                constexpr f32 kSkirtDrop = 2.5f; // m down
                const auto skirt = [&](f32 ax, f32 az, f32 bx, f32 bz,
                                       f32 ox, f32 oz) {
                    if (std::abs(ax - bx) < 1.0e-3f &&
                        std::abs(az - bz) < 1.0e-3f) {
                        return; // side fully consumed by its chamfers
                    }
                    const u32 v0 = vertex(ax, lake.level, az, 0.0f,
                                          0.0f, 0.0f, foamGate, 0.0f,
                                          1.0e6f, mat);
                    const u32 v1 = vertex(bx, lake.level, bz, 0.0f,
                                          0.0f, 0.0f, foamGate, 0.0f,
                                          1.0e6f, mat);
                    const u32 v2 = vertex(bx + ox, lake.level - kSkirtDrop,
                                          bz + oz, 0.0f, 0.0f, 0.0f,
                                          foamGate, 0.0f, 1.0e6f, mat);
                    const u32 v3 = vertex(ax + ox, lake.level - kSkirtDrop,
                                          az + oz, 0.0f, 0.0f, 0.0f,
                                          foamGate, 0.0f, 1.0e6f, mat);
                    for (const u32 k : { v0, v2, v1, v0, v3, v2 }) {
                        indices.push_back(k);
                    }
                };
                constexpr f32 kDiag = 0.70710678f;
                if (west.crest) {
                    skirt(x0, cutNW ? z0 + ck : z0, x0,
                          cutSW ? z1 - ck : z1, -kSkirtOut, 0.0f);
                }
                if (east.crest) {
                    skirt(x1, cutNE ? z0 + ck : z0, x1,
                          cutSE ? z1 - ck : z1, kSkirtOut, 0.0f);
                }
                if (north.crest) {
                    skirt(cutNW ? x0 + ck : x0, z0,
                          cutNE ? x1 - ck : x1, z0, 0.0f, -kSkirtOut);
                }
                if (south.crest) {
                    skirt(cutSW ? x0 + ck : x0, z1,
                          cutSE ? x1 - ck : x1, z1, 0.0f, kSkirtOut);
                }
                if (cutNW) {
                    skirt(x0, z0 + ck, x0 + ck, z0,
                          -kDiag * kSkirtOut, -kDiag * kSkirtOut);
                }
                if (cutNE) {
                    skirt(x1 - ck, z0, x1, z0 + ck,
                          kDiag * kSkirtOut, -kDiag * kSkirtOut);
                }
                if (cutSE) {
                    skirt(x1, z1 - ck, x1 - ck, z1,
                          kDiag * kSkirtOut, kDiag * kSkirtOut);
                }
                if (cutSW) {
                    skirt(x0 + ck, z1, x0, z1 - ck,
                          -kDiag * kSkirtOut, kDiag * kSkirtOut);
                }
            }
        }
    }
    for (const RiverSurface& river : bodies->rivers) {
        // Ribbon strip along the polyline at the water surface, slightly
        // wider than the carved bed so banks clip it. Tight bends are
        // subdivided by curvature (RiverGeometry) — the water-info
        // raster samples the same conditioned curve.
        vector<RiverNode> nodes =
            terrain::subdivideRiverNodes(river.nodes);
        // Steep reaches: the curvature subdivision alone leaves long
        // straight CHORDS between baked nodes, and the convex bed
        // between them pokes through — the lower half of a torrent
        // ran inside its channel walls (measured dev, (9622, 4131)).
        // Resample every ~4 m and GROUND each node on the RENDER
        // terrain; max() only lifts, so pools and backwaters keep
        // their chord while chutes hug their bed.
        {
            vector<RiverNode> dense;
            dense.reserve(nodes.size() * 2);
            for (size_t i = 0; i < nodes.size(); ++i) {
                if (i > 0) {
                    const RiverNode& a = nodes[i - 1];
                    const RiverNode& b = nodes[i];
                    const f32 segLen =
                        std::hypot(b.x - a.x, b.z - a.z);
                    const i32 cuts = static_cast<i32>(segLen / 4.0f);
                    for (i32 k = 1; k <= cuts; ++k) {
                        const f32 t = static_cast<f32>(k) /
                                      static_cast<f32>(cuts + 1);
                        RiverNode m;
                        m.x = glm::mix(a.x, b.x, t);
                        m.z = glm::mix(a.z, b.z, t);
                        m.surface = glm::mix(a.surface, b.surface, t);
                        m.halfWidth =
                            glm::mix(a.halfWidth, b.halfWidth, t);
                        dense.push_back(m);
                    }
                }
                dense.push_back(nodes[i]);
            }
            for (RiverNode& nd : dense) {
                // The S5d pass CARVED the bed by ~0.18 x width — the
                // water fills that channel (a 15 cm film left every
                // chute looking nearly dry between the fully-deep
                // baked nodes, dev feedback). The tier caps are not
                // published runtime-side; the generic clamp only
                // under-lifts fleuves, whose chord already carries
                // the reconciled depth (max() keeps it).
                const f32 bed = glm::clamp(
                    0.18f * 2.0f * nd.halfWidth, 0.5f, 4.0f);
                nd.surface = glm::max(
                    nd.surface,
                    terrain::height(params, nd.x, nd.z) + bed);
            }
            nodes = std::move(dense);
        }
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
}

void WaterSystem::kickLocalGeometry(const TerrainParams& params) {
    localBuildInFlight = true;
    jobs->enqueue([sharedRef = shared, bodiesRef = bodies, params,
                   gen = generation, stamp = bodiesStamp,
                   jobsRef = jobs] {
        LocalMesh out;
        out.generation = gen;
        out.stamp = stamp;
        if (!jobsRef->isStopping() && bodiesRef &&
            !(bodiesRef->lakes.empty() && bodiesRef->rivers.empty())) {
            buildLocalGeometry(*bodiesRef, params, out.verts,
                               out.indices);
        }
        sharedRef->localMesh.push(std::move(out));
    });
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

    // Local geometry: WORKER-built (the synchronous build stuttered
    // the frame on every streamed tile — see LocalMesh); main uploads
    // the finished buffers and re-kicks if the bodies moved on.
    {
        LocalMesh lm;
        while (shared->localMesh.tryPop(lm)) {
            localBuildInFlight = false;
            if (lm.generation != generation) {
                continue;
            }
            device.destroyBuffer(localIndexBuffer);
            device.destroyBuffer(localVertexBuffer);
            localIndexBuffer = {};
            localVertexBuffer = {};
            localIndexCount = 0;
            if (!lm.verts.empty() && !lm.indices.empty()) {
                localVertexBuffer = device.createBuffer(
                    { .usage = rhi::BufferUsage::Vertex,
                      .size = lm.verts.size() * sizeof(f32) },
                    lm.verts.data());
                localIndexBuffer = device.createBuffer(
                    { .usage = rhi::BufferUsage::Index,
                      .size = lm.indices.size() * sizeof(u32) },
                    lm.indices.data());
                localIndexCount = static_cast<u32>(lm.indices.size());
            }
            if (lm.stamp != bodiesStamp) {
                bodiesDirty = true; // superseded mid-build: go again
            }
        }
    }
    if (bodiesDirty && !localBuildInFlight) {
        rebuildMaterials(device);
        kickLocalGeometry(params);
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
    // While the settle gate holds, the shaders see "no sim" (w = 0):
    // the baked water stays on screen everywhere and the window keeps
    // simulating behind the curtain.
    if (!simValid || simSettling || !simSnap || simSnap->spec.n < 2) {
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
    u32 wet = 0;
    for (const f32 d : snap.depth) {
        wet += d > 0.0f ? 1u : 0u;
    }
    simWetCells = wet;
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

    // --- The ONE closed mesh, built worker-side in extractSnapshot
    // (docs/WATER-RENDER.md §2): upload verbatim.
    device.destroyBuffer(simVertexBuffer);
    device.destroyBuffer(simIndexBuffer);
    simVertexBuffer = {};
    simIndexBuffer = {};
    simIndexCount = 0;
    if (!snap.meshIndices.empty()) {
        simVertexBuffer = device.createBuffer(
            { .usage = rhi::BufferUsage::Vertex,
              .size = snap.meshVerts.size() * sizeof(f32) },
            snap.meshVerts.data());
        simIndexBuffer = device.createBuffer(
            { .usage = rhi::BufferUsage::Index,
              .size = snap.meshIndices.size() * sizeof(u32) },
            snap.meshIndices.data());
        simIndexCount = static_cast<u32>(snap.meshIndices.size());
    }

    // --- Debug volume boxes: only while the mode shows them.
    device.destroyBuffer(simBoxInstanceBuffer);
    simBoxInstanceBuffer = {};
    simBoxInstances = 0;
    if (simCfg.debugMode == 3) {
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
            simBoxInstanceBuffer = device.createBuffer(
                { .usage = rhi::BufferUsage::Vertex,
                  .size = cells.size() * sizeof(f32) },
                cells.data());
            simBoxInstances = static_cast<u32>(cells.size() / 4);
        }
    }
}

void WaterSystem::simCachePush(sptr<terrain::WaterSimState> state,
                               f32 replaceRadius, bool pushIfNoMatch) {
    if (!state || !state->valid()) {
        return;
    }
    // One entry per window footprint: an entry within replaceRadius
    // is superseded (same zone, staler water).
    bool matched = false;
    for (size_t i = 0; i < simCache.size(); ++i) {
        const auto& spec = simCache[i]->spec;
        if (spec.n == state->spec.n &&
            std::abs(spec.texelSize - state->spec.texelSize) < 1.0e-3f &&
            std::abs(spec.originX - state->spec.originX) <
                replaceRadius &&
            std::abs(spec.originZ - state->spec.originZ) <
                replaceRadius) {
            simCache.erase(simCache.begin() + static_cast<i32>(i));
            matched = true;
            break;
        }
    }
    if (!matched && !pushIfNoMatch) {
        return; // refresh-only call: never grow, never evict the trail
    }
    if (simCache.size() >= kSimCacheCap) {
        simCache.erase(simCache.begin()); // front = least recent
    }
    simCache.push_back(std::move(state));
}

void WaterSystem::simFreeze(rhi::Device& device,
                            const terrain::WaterSimSnapshot& snap,
                            f32 replaceRadius, bool pushIfNoMatch) {
    if (snap.meshIndices.empty() || snap.spec.n < 2) {
        return;
    }
    const f32 span =
        static_cast<f32>(snap.spec.n - 1) * snap.spec.texelSize;
    // One entry per footprint: an entry within replaceRadius is
    // superseded (same zone, staler water) — mirrors simCachePush.
    bool matched = false;
    for (size_t i = 0; i < simFrozen.size(); ++i) {
        if (std::abs(simFrozen[i].span - span) < 1.0e-3f &&
            std::abs(simFrozen[i].originX - snap.spec.originX) <
                replaceRadius &&
            std::abs(simFrozen[i].originZ - snap.spec.originZ) <
                replaceRadius) {
            device.destroyBuffer(simFrozen[i].vertexBuffer);
            device.destroyBuffer(simFrozen[i].indexBuffer);
            simFrozen.erase(simFrozen.begin() + static_cast<i32>(i));
            matched = true;
            break;
        }
    }
    if (!matched && !pushIfNoMatch) {
        return; // refresh-only call: never grow, never evict the trail
    }
    if (simFrozen.size() >= kSimCacheCap) {
        device.destroyBuffer(simFrozen.front().vertexBuffer);
        device.destroyBuffer(simFrozen.front().indexBuffer);
        simFrozen.erase(simFrozen.begin());
    }
    FrozenWindow fz;
    fz.vertexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex,
          .size = snap.meshVerts.size() * sizeof(f32) },
        snap.meshVerts.data());
    fz.indexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Index,
          .size = snap.meshIndices.size() * sizeof(u32) },
        snap.meshIndices.data());
    fz.indexCount = static_cast<u32>(snap.meshIndices.size());
    fz.originX = snap.spec.originX;
    fz.originZ = snap.spec.originZ;
    fz.span = span;
    simFrozen.push_back(fz);
}

void WaterSystem::simFrozenClearNow(rhi::Device& device) {
    for (FrozenWindow& fz : simFrozen) {
        device.destroyBuffer(fz.vertexBuffer);
        device.destroyBuffer(fz.indexBuffer);
    }
    simFrozen.clear();
    simFrozenClearPending = false;
}

std::array<Vec4, 4> WaterSystem::simFrozenInfo() const {
    std::array<Vec4, 4> out {};
    if (simCfg.debugMode == 1) {
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

void WaterSystem::updateSim(rhi::Device& device,
                            const TerrainParams& params,
                            const Vec3& cameraPos, f32 dt) {
    using terrain::WaterSimState;
    if (simFrozenClearPending) {
        simFrozenClearNow(device);
    }
    // Apply finished jobs (newest wins; at most one is ever in flight).
    SimResult res;
    while (shared->simDone.tryPop(res)) {
        if (res.generation != generation) {
            continue;
        }
        simInFlight = false;
        if (res.epoch != simEpoch) {
            // Invalidated while the job ran (teleport): the state is
            // still a perfectly settled window — cache it for the
            // return trip instead of dropping it, and keep its mesh
            // frozen on screen.
            if (res.snap) {
                simFreeze(device, *res.snap, simCfg.span * 0.5f, true);
            }
            simCachePush(std::move(res.state), simCfg.span * 0.5f,
                         true);
            continue;
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
            // Settle-gate calm metric: consecutive published volumes
            // within a relative epsilon = the window is calm, reveal.
            if (simSettling) {
                const f64 ref = glm::max(std::abs(simLastVolume), 1.0);
                if (simLastVolume >= 0.0 &&
                    std::abs(res.volume - simLastVolume) / ref <
                        static_cast<f64>(kSimCalmEps)) {
                    ++simCalmTicks;
                } else {
                    simCalmTicks = 0;
                }
                simLastVolume = res.volume;
                if (simCalmTicks >= kSimCalmTicks) {
                    simSettling = false;
                }
            }
        }
    }
    // The reveal never blocks: a window that keeps sloshing (heavy
    // inflow) shows after the cap regardless. Toggling the gate off
    // reveals immediately.
    if (simSettling) {
        simSettleTimer += dt;
        if (!simCfg.settleGated || simSettleTimer > kSimSettleCap) {
            simSettling = false;
        }
    }
    if (!simCfg.enabled || !params.base) {
        if (simState || simValid) {
            simState.reset();
            simSnap.reset();
            simValid = false;
            ++simEpoch;
            simAccum = 0.0f;
            simHasLastCrumb = false;
        }
        return;
    }
    // Fast travel does NOT invalidate: the window SCROLLS at any
    // speed (interior preserved bit-exactly, only entering strips
    // re-init) — dropping the state made every spectator move replay
    // the waterfall from scratch (measured by the dev). Only a true
    // TELEPORT (a jump past half a window in one frame) re-enters
    // through the pre-roll.
    const Vec2 camXz { cameraPos.x, cameraPos.z };
    Vec2 camDelta { 0.0f, 0.0f };
    if (simHasLastCam) {
        camDelta = camXz - simLastCam;
    }
    simLastCam = camXz;
    simHasLastCam = true;
    if (simState &&
        (std::abs(camDelta.x) > simCfg.span * 0.5f ||
         std::abs(camDelta.y) > simCfg.span * 0.5f)) {
        // Teleport: keep the settled window for the return trip, and
        // its mesh frozen on screen.
        if (simSnap) {
            simFreeze(device, *simSnap, simCfg.span * 0.5f, true);
        }
        simCachePush(std::move(simState), simCfg.span * 0.5f, true);
        simState.reset();
        simSnap.reset();
        simValid = false;
        ++simEpoch;
        simAccum = 0.0f;
        simHasLastCrumb = false;
    }
    if (simInFlight) {
        return;
    }
    // Main owns the state here: honor a pending dump request.
    if (!simDumpPath.empty() && simState) {
        if (terrain::dumpSimState(*simState, simCfg.params, simSrcCache,
                                  simDumpPath.c_str())) {
            LOG_INFO("Water sim: state dumped to {}", simDumpPath);
        }
        simDumpPath.clear();
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
        simHasLastCrumb = false;
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
        // Session cache first: a window evicted here earlier RESUMES
        // (scrolled by the regular path below) instead of re-running
        // the pre-roll solver — returning water is already flowing.
        {
            vector<terraingen::GridSpec> specs;
            specs.reserve(simCache.size());
            for (const auto& s : simCache) {
                specs.push_back(s->spec);
            }
            const terrain::CachedWindowPick pick =
                terrain::chooseCachedWindow(specs, spec);
            if (pick.index >= 0) {
                simState = std::move(
                    simCache[static_cast<size_t>(pick.index)]);
                simCache.erase(simCache.begin() + pick.index);
                // The ground may have re-baked while evicted: refresh
                // it on the next job; the regular path scrolls the
                // window to the new origin and re-pins the lakes.
                simGroundDirty = true;
                simSettling = simCfg.settleGated;
                simCalmTicks = 0;
                simLastVolume = -1.0;
                simSettleTimer = 0.0f;
            }
        }
    }
    if (!simState) {
        terraingen::GridSpec spec;
        spec.texelSize = texel;
        spec.n = static_cast<u32>(simCfg.span / texel) + 1;
        spec.originX =
            std::floor((cameraPos.x - simCfg.span * 0.5f) / texel) *
            texel;
        spec.originZ =
            std::floor((cameraPos.z - simCfg.span * 0.5f) / texel) *
            texel;
        simSettling = simCfg.settleGated;
        simCalmTicks = 0;
        simLastVolume = -1.0;
        simSettleTimer = 0.0f;
        simInFlight = true;
        const f32 maxX =
            spec.originX + static_cast<f32>(spec.n - 1) * texel;
        const f32 maxZ =
            spec.originZ + static_cast<f32>(spec.n - 1) * texel;
        jobs->enqueue([sharedRef = shared, params, spec,
                       simParams = simCfg.params, fn = simSourcesFn,
                       bodiesRef = bodies, gen = generation,
                       epoch = simEpoch, maxX, maxZ,
                       pinHw = simCfg.pinRiverHalfWidth,
                       jobsRef = jobs] {
            if (jobsRef->isStopping()) {
                return; // abandonable at shutdown (seconds of pre-roll)
            }
            SimResult out;
            out.generation = gen;
            out.epoch = epoch;
            const core::TimePoint start = core::clockNow();
            if (fn) {
                out.sources = fn(spec.originX, spec.originZ, maxX, maxZ);
            }
            if (bodiesRef) {
                const auto baked = bakedEntrySources(
                    *bodiesRef, spec, out.sources, simParams.rainRate,
                    pinHw);
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
                // Baked lakes = pinned reservoirs, then a settling
                // burst (~15 s of sim, well under a second of worker):
                // the offline pre-roll knows nothing of the pins, so
                // without this every (re)entry showed falls RESTARTING
                // from a dry cliff instead of already flowing.
                terrain::pinLakes(*state, *bodiesRef);
                terrain::pinRivers(*state, *bodiesRef, pinHw);
                terrain::stepWindow(*state, simParams, out.sources,
                                    450);
            }
            auto snap = std::make_shared<terrain::WaterSimSnapshot>();
            terrain::extractSnapshot(*state, simParams, *snap);
            f64 volume = 0.0;
            for (const f32 d : snap->depth) {
                volume += d;
            }
            out.volume = volume * spec.texelSize * spec.texelSize;
            out.state = std::move(state);
            out.snap = std::move(snap);
            out.millis =
                static_cast<f32>(core::secondsSince(start) * 1000.0);
            sharedRef->simDone.push(std::move(out));
        });
        return;
    }
    // Regular tick: fixed-dt accumulator (hitch-clamped), re-anchor by
    // hysteresis, ground refresh on terraforming. The time scale
    // multiplies accumulated sim-time only — per-substep dt is fixed
    // (CFL intact), and the hitch clamp scales with it so a fast sim
    // is not silently throttled back to 1x.
    const f32 timeScale = glm::clamp(simCfg.timeScale, 0.25f, 8.0f);
    simAccum += glm::min(dt, 0.25f) * timeScale;
    const f32 tickDt = glm::max(simCfg.params.dt, 1.0f / 240.0f);
    u32 substeps = static_cast<u32>(simAccum / tickDt);
    substeps = glm::min(
        substeps,
        static_cast<u32>(glm::ceil(
            static_cast<f32>(simCfg.maxSubsteps) *
            glm::max(timeScale, 1.0f))));
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
    // Breadcrumb drop: every half-window of TRAVEL (scroll never
    // evicts, so without this the cache only ever saw teleports), a
    // copy of the current state joins the cache (~2.4 MB, sub-ms).
    if (!simHasLastCrumb) {
        simLastCrumb = { spec.originX, spec.originZ };
        simHasLastCrumb = true;
    } else if (std::abs(spec.originX - simLastCrumb.x) >
                   simCfg.span * 0.5f ||
               std::abs(spec.originZ - simLastCrumb.y) >
                   simCfg.span * 0.5f) {
        simCachePush(std::make_shared<WaterSimState>(*simState),
                     simCfg.span * 0.5f, true);
        if (simSnap) {
            // Trail stays visible behind.
            simFreeze(device, *simSnap, simCfg.span * 0.5f, true);
        }
        simLastCrumb = { spec.originX, spec.originZ };
        simFreshTimer = 0.0f;
    } else {
        // Timed refresh of the CURRENT footprint (see the member
        // comment: dynamic-only water formed since the last drop must
        // reach the crumb and the frozen mesh before it is evicted;
        // faster sim = faster cadence). REPLACE-ONLY, tight radius:
        // with the travel-wide radius it superseded the previous
        // trail entry every few seconds of walking — ONE entry slid
        // along with the player and the waterfall behind vanished
        // past ~half a window (measured dev). While moving, the
        // jalons own the trail (a jalon's window still covers the
        // 256 m behind it, so water formed on the move is captured).
        simFreshTimer += dt * glm::max(timeScale, 1.0f);
        if (simFreshTimer >= kSimFreshSeconds) {
            simFreshTimer = 0.0f;
            simCachePush(std::make_shared<WaterSimState>(*simState),
                         kSimFreshRadius, false);
            if (simSnap) {
                simFreeze(device, *simSnap, kSimFreshRadius, false);
            }
        }
    }
    // Crumbs for the entering strips (immutable while the job reads
    // them: one job in flight, and a cache pop can only happen after
    // it lands).
    vector<sptr<const WaterSimState>> crumbs;
    if (dCol != 0 || dRow != 0) {
        crumbs.reserve(simCache.size());
        for (const auto& c : simCache) {
            crumbs.push_back(c);
        }
    }
    simInFlight = true;
    const bool refreshGround = simGroundDirty;
    simGroundDirty = false;
    jobs->enqueue([sharedRef = shared, params, state = simState,
                   simParams = simCfg.params, fn = simSourcesFn,
                   bodiesRef = bodies, sources = simSrcCache,
                   gen = generation, epoch = simEpoch, dCol, dRow,
                   substeps, refreshGround,
                   crumbs = std::move(crumbs),
                   pinHw = simCfg.pinRiverHalfWidth,
                   jobsRef = jobs]() mutable {
        if (jobsRef->isStopping()) {
            return; // abandonable at shutdown
        }
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
                                  simParams.seaLevel,
                                  crumbs.empty() ? nullptr : &crumbs);
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
                    *bodiesRef, sp, sources, simParams.rainRate,
                    pinHw);
                sources.insert(sources.end(), baked.begin(),
                               baked.end());
            }
            out.sourcesFresh = true;
        }
        if (bodiesRef && (refreshGround || dCol != 0 || dRow != 0)) {
            // Ground or window moved: re-rasterize the reservoirs (and
            // apply them — at least one substep).
            terrain::pinLakes(*state, *bodiesRef);
            terrain::pinRivers(*state, *bodiesRef, pinHw);
            substeps = glm::max(substeps, 1u);
        }
        terrain::stepWindow(*state, simParams, sources, substeps);
        auto snap = std::make_shared<terrain::WaterSimSnapshot>();
        terrain::extractSnapshot(*state, simParams, *snap);
        f64 volume = 0.0;
        for (const f32 d : snap->depth) {
            volume += d;
        }
        out.volume =
            volume * state->spec.texelSize * state->spec.texelSize;
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
              { { .stride = 5 * sizeof(f32),
                  .attributes = { { .location = 0,
                                    .format = rhi::VertexFormat::F32x3,
                                    .offset = 0 },
                                  { .location = 1,
                                    .format = rhi::VertexFormat::F32x2,
                                    .offset = 3 * sizeof(f32) } } } },
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Greater },
          .cull = rhi::CullMode::None,
          .depthBias = 4.0f,
          .depthBiasSlope = 2.5f });
    if (simFrozenPipeline.id != 0) {
        device.destroyPipeline(simFrozenPipeline);
    }
    // Frozen windows: same mesh layout and states, frozen shading.
    simFrozenPipeline = device.createPipeline(
        { .shader = shaders.get(kWaterSimFrozenShader),
          .vertexBuffers =
              { { .stride = 5 * sizeof(f32),
                  .attributes = { { .location = 0,
                                    .format = rhi::VertexFormat::F32x3,
                                    .offset = 0 },
                                  { .location = 1,
                                    .format = rhi::VertexFormat::F32x2,
                                    .offset = 3 * sizeof(f32) } } } },
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Greater },
          .cull = rhi::CullMode::None,
          .depthBias = 4.0f,
          .depthBiasSlope = 2.5f });
    if (simPipelineOverlay.id != 0) {
        device.destroyPipeline(simPipelineOverlay);
    }
    // Seam overlay: depth test OFF — the diagnosis view.
    simPipelineOverlay = device.createPipeline(
        { .shader = shaders.get(kWaterSimShader),
          .vertexBuffers =
              { { .stride = 5 * sizeof(f32),
                  .attributes = { { .location = 0,
                                    .format = rhi::VertexFormat::F32x3,
                                    .offset = 0 },
                                  { .location = 1,
                                    .format = rhi::VertexFormat::F32x2,
                                    .offset = 3 * sizeof(f32) } } } },
          .depth = { .testEnable = false, .writeEnable = false },
          .cull = rhi::CullMode::None });
    if (simBoxPipeline.id != 0) {
        device.destroyPipeline(simBoxPipeline);
    }
    // Debug volume boxes: instanced translucent columns.
    simBoxPipeline = device.createPipeline(
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
          .depth = { .testEnable = true,
                     .writeEnable = false,
                     .compare = rhi::CompareFunc::Greater },
          .cull = rhi::CullMode::None });
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
    if (simValid && !simSettling && simIndexCount > 0 &&
        simCfg.debugMode != 1) {
        // The live sim sheet (drawn last: its fragments discard where
        // dry, the baked ones discard where the sim owns the texel).
        // Settle gate: while settling, only the baked water shows
        // (simMapInfo already reads invalid) — the mesh stays hidden
        // with it. Seam overlay renders WITHOUT depth test: the
        // unambiguous "where does the sim have water" view.
        cmd.setPipeline(simCfg.debugMode == 2 ? simPipelineOverlay
                                              : simPipeline);
        cmd.setBindGroup(0, frameBindGroup);
        cmd.setBindGroup(1, sceneBindGroup);
        cmd.setBindGroup(2, poolMapGroup);
        cmd.setVertexBuffer(0, simVertexBuffer);
        cmd.setIndexBuffer(simIndexBuffer, rhi::IndexFormat::U32);
        cmd.drawIndexed(simIndexCount);
    }
    if (!simFrozen.empty() && simCfg.debugMode != 1) {
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
        if (simCfg.debugMode == 3 && simBoxInstances > 0) {
            // Debug volume columns: "where the water IS".
            cmd.setPipeline(simBoxPipeline);
            cmd.setBindGroup(0, frameBindGroup);
            cmd.setVertexBuffer(0, simBoxVertexBuffer);
            cmd.setVertexBuffer(1, simBoxInstanceBuffer);
            cmd.setIndexBuffer(simBoxIndexBuffer, rhi::IndexFormat::U16);
            cmd.drawIndexed(36, simBoxInstances);
        }
    }
}

} // namespace render
