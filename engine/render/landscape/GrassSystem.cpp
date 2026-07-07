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
// 7.8quinquies (dev): the patch DISTRIBUTION across the map is right —
// what was missing is the volume INSIDE a patch. A BotW clump reads as
// one SOLID mass of grass with blades poking out of its top and sides,
// so inside the mask the grid is packed tight (blades overlap); the
// distance density LOD in draw() keeps the vertex budget in check.
constexpr f32 kBladeSpacing = 0.15f; // meters between candidates (in-patch)

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
            // Near-BINARY presence (7.8quinquies): even moderately inside
            // the mask the clump is at FULL density (the solid volume);
            // only the rim thins, fast, so patches keep their silhouette.
            const f32 patch = patchMask(params.seed, x, z);
            const f32 presence = glm::smoothstep(0.08f, 0.40f, patch);
            if (presence < 0.02f || rng.next() >= presence) {
                continue;
            }
            const f32 h = terrain::height(params, x, z);
            const Vec3 n = terrain::normal(params, x, z);
            // HARD material cutoff: grass only on solidly grassy ground —
            // never on the sand/snow/rock transition fringes.
            if (terrain::materialWeights(params, h, n).grass < 0.72f) {
                continue;
            }
            // Height: near-uniform inside the volume so the top reads as
            // one surface; the rim droops shorter, and ~12% of blades
            // overshoot — the tips poking above the mass (the BotW tell).
            f32 scale =
                (0.70f + rng.next() * 0.30f) * (0.55f + 0.45f * presence);
            if (rng.next() < 0.12f) {
                scale *= 1.35f;
            }
            // Rim blades lean/curve harder — the clump spills over its
            // sides instead of ending in a wall.
            const f32 lean =
                glm::min(1.0f, rng.next() + (1.0f - presence) * 0.6f);
            result.push_back({
                .positionScale = { x, h, z, scale },
                .params = { rng.next() * 6.2831853f,   // yaw
                            rng.next() * 6.2831853f,   // flutter phase
                            rng.next(),                // tint jitter
                            lean },                    // lean amount
                .groundNormal = { n, 0.0f },           // 7.8bis: BotW shading
            });
        }
    }
    // Sort by the density-LOD keep key (flutter phase — uniform random
    // per blade): grass.vert clips blades whose key exceeds the metric
    // density curve, so a PREFIX of this buffer is EXACTLY the set the
    // shader keeps. draw() cuts the prefix with the same curve — the
    // vertex shader never even sees the tail it would have clipped.
    std::sort(result.begin(), result.end(),
              [](const GrassSystem::Instance& a,
                 const GrassSystem::Instance& b) {
                  return a.params.y < b.params.y;
              });
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
                                    .offset = offsetof(Instance, params) },
                                  { .location = 4,
                                    .format = rhi::VertexFormat::F32x4,
                                    .offset = offsetof(
                                        Instance, groundNormal) } } } },
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
                       const Vec3& cameraPos, const Frustum* frustum) {
    cmd.setPipeline(pipeline);
    cmd.setBindGroup(0, frameBindGroup);
    if (shadowBindGroup.id != 0) {
        cmd.setBindGroup(2, shadowBindGroup);
    }
    cmd.setVertexBuffer(0, bladeVertexBuffer);
    cmd.setIndexBuffer(bladeIndexBuffer, rhi::IndexFormat::U16);
    struct Draw {
        f32 nearest;
        const Chunk* chunk;
        u32 count;
    };
    vector<Draw> draws;
    draws.reserve(chunks.size());
    for (const auto& [key, chunk] : chunks) {
        if (!chunk.resident || chunk.instanceCount == 0) {
            continue;
        }
        const f32 minX = static_cast<f32>(static_cast<i32>(key >> 32)) *
                         TerrainSystem::kChunkSize;
        const f32 minZ =
            static_cast<f32>(static_cast<i32>(key & 0xffffffffu)) *
            TerrainSystem::kChunkSize;
        const f32 dx = glm::max(
            glm::max(minX - cameraPos.x,
                     cameraPos.x - (minX + TerrainSystem::kChunkSize)),
            0.0f);
        const f32 dz = glm::max(
            glm::max(minZ - cameraPos.z,
                     cameraPos.z - (minZ + TerrainSystem::kChunkSize)),
            0.0f);
        const f32 nearest = std::sqrt(dx * dx + dz * dz);
        if (nearest > kFadeEnd) {
            continue; // every blade fully sunk into the ground
        }
        if (frustum) {
            // +1.5 m headroom over the blade roots (height × scale + wind).
            if (!frustum->intersectsAabb(
                    { minX, chunk.minY - 0.5f, minZ },
                    { minX + TerrainSystem::kChunkSize, chunk.maxY + 1.5f,
                      minZ + TerrainSystem::kChunkSize })) {
                continue;
            }
        }
        // Density LOD prefix: instances are SORTED by the keep key, so
        // drawing the first N is exactly the set grass.vert keeps at
        // this chunk's NEAREST distance (density is monotonic in dist —
        // every blade in the chunk is at least that far). Same curve as
        // the shader: density = mix(1.0, 0.24, smoothstep(18, 110, d)),
        // +3% margin for the key distribution's sampling noise.
        const f32 thin = glm::smoothstep(18.0f, 110.0f, nearest);
        const f32 density = glm::min(1.0f, 1.0f - 0.76f * thin + 0.03f);
        const u32 count = glm::min(
            chunk.instanceCount,
            1u + static_cast<u32>(
                     static_cast<f32>(chunk.instanceCount) * density));
        draws.push_back({ nearest, &chunk, count });
    }
    // Front-to-back: near blades fill the depth buffer first, so the
    // grass behind them early-z-rejects instead of shading over.
    std::sort(draws.begin(), draws.end(),
              [](const Draw& a, const Draw& b) {
                  return a.nearest < b.nearest;
              });
    for (const Draw& d : draws) {
        cmd.setVertexBuffer(1, d.chunk->instanceBuffer);
        cmd.drawIndexed(bladeIndexCount, d.count);
    }
}

} // namespace render
