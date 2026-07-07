#include "engine/render/landscape/GrassSystem.hpp"

#include <algorithm>
#include <cmath>

#include "engine/core/Jobs.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/render/landscape/TerrainSystem.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {

constexpr const char* kGrassShader = "grass";
// 7.8 follow-up (dev: "many more, thinner blades" — the reference packs
// one blade per ~0.1 units): doubled density; push lower with an eye on
// the Release FPS, the instance count scales as 1/spacing².
constexpr f32 kBladeSpacing = 0.27f; // meters between candidates (in-patch)

// BotW-style patch layout: grass gathers in dense clumps with bare meadow
// between them. Two noise scales — broad patches and small clump detail —
// thresholded into a [0,1] density mask (0 = bare, 1 = heart of a patch).
f32 patchMask(u32 seed, f32 x, f32 z) {
    const f32 broad = terrain::noise01(seed ^ 0x1f123bb5u, x / 21.0f,
                                       z / 21.0f);
    const f32 detail = terrain::noise01(seed ^ 0x9d2c5680u, x / 6.0f,
                                        z / 6.0f);
    return glm::smoothstep(0.47f, 0.60f, broad * 0.72f + detail * 0.28f);
}

u32 hashU32(u32 v) {
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    return v;
}

// Small deterministic per-candidate random stream.
struct HashRng {
    u32 state;
    f32 next() { // [0, 1)
        state = hashU32(state);
        return static_cast<f32>(state) * (1.0f / 4294967296.0f);
    }
};

// One blade = a tapered 7-triangle ribbon, REAL geometry (no alpha test):
// vertex = (side in [-1,1] with the taper baked in, t along the blade).
// 7.8: matched to the daniel-ilett reference — BLADE_SEGMENTS 4 with a
// LINEAR taper to a sharp point (width × (1 - t)), which is what reads
// as "thin blades"; the vertex shader gives it width, height, curvature
// and wind.
constexpr f32 kBladeVertices[] = {
    // side,  t
    -1.00f, 0.00f,
     1.00f, 0.00f,
    -0.75f, 0.25f,
     0.75f, 0.25f,
    -0.50f, 0.50f,
     0.50f, 0.50f,
    -0.25f, 0.75f,
     0.25f, 0.75f,
     0.00f, 1.00f, // the point
};
constexpr u16 kBladeIndices[] = {
    0, 1, 2,  1, 3, 2,  2, 3, 4,  3, 5, 4,
    4, 5, 6,  5, 7, 6,  6, 7, 8,
};

} // namespace

vector<GrassSystem::Instance> scatterGrass(const TerrainParams& params,
                                           i32 cx, i32 cz) {
    const f32 originX = static_cast<f32>(cx) * TerrainSystem::kChunkSize;
    const f32 originZ = static_cast<f32>(cz) * TerrainSystem::kChunkSize;
    const u32 perSide =
        static_cast<u32>(TerrainSystem::kChunkSize / kBladeSpacing);

    vector<GrassSystem::Instance> result;
    result.reserve(perSide * perSide / 2);
    for (u32 gz = 0; gz < perSide; ++gz) {
        for (u32 gx = 0; gx < perSide; ++gx) {
            HashRng rng { hashU32(params.seed ^ 0x6b79a3f1u) ^
                          hashU32(static_cast<u32>(cx * 73856093 ^
                                                   cz * 19349663) ^
                                  (gz * perSide + gx)) };
            const f32 x = originX + (static_cast<f32>(gx) + rng.next()) *
                                        kBladeSpacing;
            const f32 z = originZ + (static_cast<f32>(gz) + rng.next()) *
                                        kBladeSpacing;
            // Patch mask first (cheapest test): most candidates die here,
            // leaving dense clumps separated by bare ground.
            const f32 patch = patchMask(params.seed, x, z);
            if (patch < 0.03f || rng.next() >= patch) {
                continue;
            }
            const f32 h = terrain::height(params, x, z);
            const Vec3 n = terrain::normal(params, x, z);
            // HARD material cutoff: grass only on solidly grassy ground —
            // never on the sand/snow/rock transition fringes.
            if (terrain::materialWeights(params, h, n).grass < 0.72f) {
                continue;
            }
            // Blades stand taller toward the heart of a patch.
            const f32 scale =
                (0.55f + rng.next() * 0.55f) * (0.72f + 0.42f * patch);
            result.push_back({
                .positionScale = { x, h, z, scale },
                .params = { rng.next() * 6.2831853f,   // yaw
                            rng.next() * 6.2831853f,   // flutter phase
                            rng.next(),                // tint jitter
                            rng.next() },              // lean amount
            });
        }
    }
    return result;
}

void GrassSystem::create(rhi::Device& device, ShaderLibrary& shaders,
                         core::JobSystem& jobSystem) {
    jobs = &jobSystem;
    shared = std::make_shared<Shared>();

    bladeVertexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Vertex, .size = sizeof(kBladeVertices) },
        kBladeVertices);
    bladeIndexBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Index, .size = sizeof(kBladeIndices) },
        kBladeIndices);
    bladeIndexCount = static_cast<u32>(std::size(kBladeIndices));

    shaders.load(kGrassShader, { { "FrameUbo", 0 } },
                 { { "uShadowMap", 1 } });
    buildPipeline(device, shaders);
}

void GrassSystem::destroy(rhi::Device& device) {
    ++generation;
    for (auto& [key, chunk] : chunks) {
        device.destroyBuffer(chunk.instanceBuffer);
    }
    chunks.clear();
    instances = 0;
    device.destroyPipeline(pipeline);
    device.destroyBuffer(bladeIndexBuffer);
    device.destroyBuffer(bladeVertexBuffer);
    pipeline = {};
    bladeIndexBuffer = {};
    bladeVertexBuffer = {};
}

void GrassSystem::regenerate(rhi::Device& device) {
    ++generation;
    for (auto& [key, chunk] : chunks) {
        device.destroyBuffer(chunk.instanceBuffer);
    }
    chunks.clear();
    instances = 0;
}

void GrassSystem::update(rhi::Device& device, const TerrainParams& params,
                         const Vec3& cameraPos) {
    // Budgeted uploads.
    u32 uploads = 0;
    BuiltChunk built;
    while (uploads < kMaxUploadsPerFrame && shared->built.tryPop(built)) {
        if (built.generation != generation) {
            continue;
        }
        const auto it = chunks.find(keyOf(built.cx, built.cz));
        if (it == chunks.end() || it->second.resident) {
            continue;
        }
        Chunk& chunk = it->second;
        chunk.instanceCount = static_cast<u32>(built.instances.size());
        if (chunk.instanceCount > 0) {
            chunk.instanceBuffer = device.createBuffer(
                { .usage = rhi::BufferUsage::Vertex,
                  .size = built.instances.size() * sizeof(Instance) },
                built.instances.data());
            chunk.minY = built.instances[0].positionScale.y;
            chunk.maxY = chunk.minY;
            for (const Instance& instance : built.instances) {
                chunk.minY = glm::min(chunk.minY, instance.positionScale.y);
                chunk.maxY = glm::max(chunk.maxY, instance.positionScale.y);
            }
        }
        chunk.resident = true;
        instances += chunk.instanceCount;
        ++uploads;
    }

    // Request missing chunks in the grass ring — nearest first, budgeted
    // (the rest is re-detected next frame; the state IS the queue).
    const i32 camCx = static_cast<i32>(
        std::floor(cameraPos.x / TerrainSystem::kChunkSize));
    const i32 camCz = static_cast<i32>(
        std::floor(cameraPos.z / TerrainSystem::kChunkSize));
    struct Candidate {
        i32 cx, cz, dist2;
    };
    vector<Candidate> wanted;
    for (i32 dz = -kViewRadius; dz <= kViewRadius; ++dz) {
        for (i32 dx = -kViewRadius; dx <= kViewRadius; ++dx) {
            const i32 cx = camCx + dx;
            const i32 cz = camCz + dz;
            if (chunks.contains(keyOf(cx, cz))) {
                continue;
            }
            wanted.push_back({ cx, cz, dx * dx + dz * dz });
        }
    }
    std::sort(wanted.begin(), wanted.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.dist2 < b.dist2;
              });
    u32 requests = 0;
    for (const Candidate& c : wanted) {
        if (requests >= kMaxRequestsPerFrame) {
            break;
        }
        const i32 cx = c.cx;
        const i32 cz = c.cz;
        chunks.emplace(keyOf(cx, cz), Chunk {});
        jobs->enqueue([sharedRef = shared, params, cx, cz,
                       gen = generation] {
            sharedRef->built.push(
                { cx, cz, gen, scatterGrass(params, cx, cz) });
        });
        ++requests;
    }

    // Evict beyond hysteresis.
    for (auto it = chunks.begin(); it != chunks.end();) {
        const i32 cx = static_cast<i32>(it->first >> 32);
        const i32 cz = static_cast<i32>(it->first & 0xffffffffu);
        if (std::max(std::abs(cx - camCx), std::abs(cz - camCz)) <=
            kEvictRadius) {
            ++it;
            continue;
        }
        if (it->second.resident) {
            device.destroyBuffer(it->second.instanceBuffer);
            instances -= it->second.instanceCount;
        }
        it = chunks.erase(it);
    }
}

void GrassSystem::buildPipeline(rhi::Device& device, ShaderLibrary& shaders) {
    if (pipeline.id != 0) {
        device.destroyPipeline(pipeline);
    }
    pipeline = device.createPipeline(
        { .shader = shaders.get(kGrassShader),
          .vertexBuffers =
              { { .stride = 2 * sizeof(f32),
                  .attributes = { { .location = 0,
                                    .format = rhi::VertexFormat::F32x2,
                                    .offset = 0 } } },
                { .stride = sizeof(Instance),
                  .stepMode = rhi::VertexStepMode::Instance,
                  .attributes = { { .location = 2,
                                    .format = rhi::VertexFormat::F32x4,
                                    .offset = offsetof(Instance,
                                                       positionScale) },
                                  { .location = 3,
                                    .format = rhi::VertexFormat::F32x4,
                                    .offset = offsetof(Instance, params) } } } },
          // Pure geometry, opaque: depth-write on so blades sort against the
          // terrain and each other; ribbons visible from both sides.
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Less },
          .cull = rhi::CullMode::None });
    shaderGeneration = shaders.generation(kGrassShader);
}

void GrassSystem::refreshPipeline(rhi::Device& device,
                                  ShaderLibrary& shaders) {
    if (shaders.generation(kGrassShader) != shaderGeneration) {
        buildPipeline(device, shaders);
    }
}

void GrassSystem::draw(rhi::CommandBuffer& cmd,
                       rhi::BindGroupHandle frameBindGroup,
                       rhi::BindGroupHandle shadowBindGroup,
                       const Frustum* frustum) {
    cmd.setPipeline(pipeline);
    cmd.setBindGroup(0, frameBindGroup);
    if (shadowBindGroup.id != 0) {
        cmd.setBindGroup(2, shadowBindGroup);
    }
    cmd.setVertexBuffer(0, bladeVertexBuffer);
    cmd.setIndexBuffer(bladeIndexBuffer, rhi::IndexFormat::U16);
    for (const auto& [key, chunk] : chunks) {
        if (!chunk.resident || chunk.instanceCount == 0) {
            continue;
        }
        if (frustum) {
            const f32 x0 = static_cast<f32>(static_cast<i32>(key >> 32)) *
                           TerrainSystem::kChunkSize;
            const f32 z0 =
                static_cast<f32>(static_cast<i32>(key & 0xffffffffu)) *
                TerrainSystem::kChunkSize;
            // +1.5 m headroom over the blade roots (height × scale + wind).
            if (!frustum->intersectsAabb(
                    { x0, chunk.minY - 0.5f, z0 },
                    { x0 + TerrainSystem::kChunkSize, chunk.maxY + 1.5f,
                      z0 + TerrainSystem::kChunkSize })) {
                continue;
            }
        }
        cmd.setVertexBuffer(1, chunk.instanceBuffer);
        cmd.drawIndexed(bladeIndexCount, chunk.instanceCount);
    }
}

} // namespace render
