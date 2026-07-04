#include "engine/render/landscape/WaterSystem.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/Jobs.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {

constexpr const char* kWaterShader = "water";

// Worker-side pool-depth bake: vertical depth below sea level, then a
// separable max-dilation (~36 m) so one texture tap answers "how deep does
// this POOL get nearby" — the small-pool foam criterion.
vector<f32> bakePoolDepth(const TerrainParams& params, Vec2 center) {
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
            depth[static_cast<size_t>(y) * kSize + x] = glm::max(
                params.seaLevel - terrain::height(params, wx, wz), 0.0f);
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
    poolMapGroup = device.createBindGroup(
        { .entries = { { .binding = 3,
                         .texture = poolMap,
                         .sampler = poolMapSampler } } });

    shaders.load(kWaterShader, { { "FrameUbo", 0 } },
                 { { "uSceneColor", 0 },
                   { "uSceneDepth", 1 },
                   { "uPoolDepth", 3 } });
    buildPipeline(device, shaders);
}

void WaterSystem::destroy(rhi::Device& device) {
    ++generation; // in-flight bakes die on arrival (shared queue outlives us)
    device.destroyBindGroup(poolMapGroup);
    device.destroyTexture(poolMap);
    device.destroySampler(poolMapSampler);
    device.destroyPipeline(pipeline);
    device.destroyBuffer(indexBuffer);
    device.destroyBuffer(vertexBuffer);
    poolMapGroup = {};
    poolMap = {};
    poolMapSampler = {};
    pipeline = {};
    indexBuffer = {};
    vertexBuffer = {};
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
        device.destroyBindGroup(poolMapGroup);
        device.destroyTexture(poolMap);
        poolMap = device.createTexture(
            { .width = kPoolMapSize,
              .height = kPoolMapSize,
              .format = rhi::TextureFormat::R16F,
              .filter = rhi::FilterMode::Linear,
              .usage = rhi::TextureUsage_Sampled },
            baked.texels.data());
        poolMapGroup = device.createBindGroup(
            { .entries = { { .binding = 3,
                             .texture = poolMap,
                             .sampler = poolMapSampler } } });
        mapCenter = baked.center;
        bakedSeed = baked.seed;
        bakedSeaLevel = baked.seaLevel;
        bakeInFlight = false;
    }

    const Vec2 camXz { cameraPos.x, cameraPos.z };
    const bool stale = glm::distance(camXz, mapCenter) > kRebakeDistance ||
                       bakedSeed != params.seed ||
                       bakedSeaLevel != params.seaLevel;
    if (stale && !bakeInFlight) {
        bakeInFlight = true;
        constexpr f32 kTexel = kPoolMapSpan / kPoolMapSize;
        const Vec2 center = glm::floor(camXz / kTexel) * kTexel;
        jobs->enqueue([sharedRef = shared, params, center,
                       gen = generation] {
            sharedRef->baked.push({ center, gen, params.seed,
                                    params.seaLevel,
                                    bakePoolDepth(params, center) });
        });
    }
}

void WaterSystem::buildPipeline(rhi::Device& device, ShaderLibrary& shaders) {
    if (pipeline.id != 0) {
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
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Less },
          .cull = rhi::CullMode::None });
    shaderGeneration = shaders.generation(kWaterShader);
}

void WaterSystem::refreshPipeline(rhi::Device& device,
                                  ShaderLibrary& shaders) {
    if (shaders.generation(kWaterShader) != shaderGeneration) {
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
}

} // namespace render
