#include "engine/render/landscape/ChunkOcclusion.hpp"

#include <cmath>

#include <glm/glm.hpp>

#include "engine/core/Jobs.hpp"
#include "engine/render/landscape/TerrainSystem.hpp"

namespace render {

namespace {

constexpr f32 kTau = 6.2831853f;

} // namespace

ChunkOcclusion::Result computeOcclusion(const ChunkOcclusion::Input& input) {
    ChunkOcclusion::Result result;
    result.generation = input.generation;

    const Vec3 cam = input.cameraPos;

    // Horizon table: running max slope per (azimuth ray, distance ring),
    // from exact terrain heights along each ray.
    constexpr u32 kRays = ChunkOcclusion::kRayCount;
    constexpr u32 kRings = ChunkOcclusion::kRingCount;
    constexpr f32 kStep = ChunkOcclusion::kRingStep;
    static_assert(kRays > 0 && kRings > 0);
    vector<f32> horizon(static_cast<size_t>(kRays) * kRings);
    for (u32 ray = 0; ray < kRays; ++ray) {
        const f32 azimuth = (static_cast<f32>(ray) + 0.5f) *
                            (kTau / static_cast<f32>(kRays));
        const f32 dirX = std::cos(azimuth);
        const f32 dirZ = std::sin(azimuth);
        f32 maxSlope = -1e9f;
        for (u32 ring = 0; ring < kRings; ++ring) {
            const f32 d = kStep * static_cast<f32>(ring + 1);
            const f32 h = terrain::height(input.params, cam.x + dirX * d,
                                          cam.z + dirZ * d);
            maxSlope = glm::max(maxSlope, (h - cam.y) / d);
            horizon[ray * kRings + ring] = maxSlope;
        }
    }

    // Judge every chunk we know the top of.
    for (const auto& [key, maxY] : input.chunkTops) {
        const i32 cx = static_cast<i32>(key >> 32);
        const i32 cz = static_cast<i32>(static_cast<u32>(key));
        const f32 x0 = static_cast<f32>(cx) * TerrainSystem::kChunkSize;
        const f32 z0 = static_cast<f32>(cz) * TerrainSystem::kChunkSize;
        const f32 x1 = x0 + TerrainSystem::kChunkSize;
        const f32 z1 = z0 + TerrainSystem::kChunkSize;

        // Nearest point of the footprint (2D): shortest sight distance.
        const f32 nx = glm::clamp(cam.x, x0, x1);
        const f32 nz = glm::clamp(cam.z, z0, z1);
        const f32 dNear = std::hypot(nx - cam.x, nz - cam.z);
        if (dNear < 3.0f * kStep) {
            continue; // close chunks: never occlusion-culled
        }

        // Steepest (most visible) slope the chunk's highest drawable point
        // can present: above the camera the nearest distance maximizes it,
        // below the camera the FARTHEST corner does (least negative).
        const f32 relTop = maxY + ChunkOcclusion::kPropHeadroom - cam.y;
        f32 dFar = 0.0f;
        {
            const f32 fx = glm::max(std::abs(x0 - cam.x),
                                    std::abs(x1 - cam.x));
            const f32 fz = glm::max(std::abs(z0 - cam.z),
                                    std::abs(z1 - cam.z));
            dFar = std::hypot(fx, fz);
        }
        const f32 topSlope = relTop >= 0.0f ? relTop / dNear : relTop / dFar;

        // The horizon must have formed strictly BEFORE the chunk.
        const i32 ringLimit =
            static_cast<i32>(dNear / kStep) - 2; // one ring + rounding short
        if (ringLimit < 0) {
            continue;
        }
        const u32 ring = glm::min(static_cast<u32>(ringLimit), kRings - 1);

        // Azimuth span of the four corners around the center direction
        // (deltas wrapped to [-pi, pi] to dodge the seam).
        const f32 centerAz = std::atan2((z0 + z1) * 0.5f - cam.z,
                                        (x0 + x1) * 0.5f - cam.x);
        f32 maxDelta = 0.0f;
        const f32 cornersX[4] = { x0, x1, x0, x1 };
        const f32 cornersZ[4] = { z0, z0, z1, z1 };
        for (u32 i = 0; i < 4; ++i) {
            f32 delta = std::atan2(cornersZ[i] - cam.z, cornersX[i] - cam.x) -
                        centerAz;
            if (delta > kTau * 0.5f) { delta -= kTau; }
            if (delta < -kTau * 0.5f) { delta += kTau; }
            maxDelta = glm::max(maxDelta, std::abs(delta));
        }

        // Min horizon over the covered rays, one ray of margin each side.
        const f32 rayWidth = kTau / static_cast<f32>(kRays);
        const i32 rayCenter = static_cast<i32>(
            std::floor((centerAz < 0.0f ? centerAz + kTau : centerAz) /
                       rayWidth));
        const i32 raySpan = static_cast<i32>(maxDelta / rayWidth) + 1;
        f32 minHorizon = 1e9f;
        for (i32 r = rayCenter - raySpan; r <= rayCenter + raySpan; ++r) {
            const u32 wrapped =
                static_cast<u32>((r % static_cast<i32>(kRays) + kRays) %
                                 kRays);
            minHorizon = glm::min(minHorizon, horizon[wrapped * kRings + ring]);
        }

        if (topSlope < minHorizon) {
            result.occluded.insert(key);
        }
    }
    return result;
}

void ChunkOcclusion::create(core::JobSystem& jobSystem) {
    jobs = &jobSystem;
    shared = std::make_shared<Shared>();
}

void ChunkOcclusion::pump() {
    Result result;
    while (shared->results.tryPop(result)) {
        inFlight = false;
        if (result.generation != generation) {
            continue; // invalidated while the worker ran
        }
        occluded = std::move(result.occluded);
    }
}

bool ChunkOcclusion::wantsRebuild(const Vec3& cameraPos) const {
    return !inFlight &&
           glm::distance(cameraPos, lastRebuildPos) > kRebuildDistance;
}

void ChunkOcclusion::rebuild(const TerrainParams& params,
                             const Vec3& cameraPos,
                             std::unordered_map<u64, f32> chunkTops) {
    inFlight = true;
    lastRebuildPos = cameraPos;
    Input input { params, cameraPos, std::move(chunkTops), generation };
    jobs->enqueue([sharedRef = shared, in = std::move(input)] {
        sharedRef->results.push(computeOcclusion(in));
    });
}

void ChunkOcclusion::invalidate() {
    ++generation;
    occluded.clear();
    lastRebuildPos = { 1e9f, 1e9f, 1e9f };
}

} // namespace render
