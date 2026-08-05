#include "game/CliffCollision.hpp"

#include <cmath>

#include <glm/gtc/quaternion.hpp>

#include "engine/render/landscape/TerrainSystem.hpp"

namespace game {

namespace {

u64 packChunk(i32 x, i32 z) {
    return (static_cast<u64>(static_cast<u32>(x)) << 32) |
           static_cast<u64>(static_cast<u32>(z));
}

i32 unpackX(u64 key) { return static_cast<i32>(key >> 32); }
i32 unpackZ(u64 key) { return static_cast<i32>(key & 0xFFFFFFFFu); }

} // namespace

CliffCollision::CliffCollision(phys::PhysicsWorld& physics,
                               const render::TerrainParams* params)
    : physics { physics }, params { params } {}

CliffCollision::~CliffCollision() { clearAll(); }

void CliffCollision::clearAll() {
    for (const auto& [key, chunkBodies] : chunks) {
        for (const phys::BodyId body : chunkBodies) {
            physics.removeBody(body);
        }
    }
    chunks.clear();
    bodies = 0;
}

void CliffCollision::cookChunk(i32 cx, i32 cz) {
    vector<phys::BodyId>& out = chunks[packChunk(cx, cz)];
    if (!params->base) {
        return;
    }
    const f32 chunkSize = render::TerrainSystem::kChunkSize;
    const f32 minX = static_cast<f32>(cx) * chunkSize;
    const f32 minZ = static_cast<f32>(cz) * chunkSize;
    const f32 maxX = minX + chunkSize;
    const f32 maxZ = minZ + chunkSize;
    for (const render::TerrainRegion& region : params->base->regions) {
        if (region.cliffBands.empty() ||
            !region.contains((minX + maxX) * 0.5f,
                             (minZ + maxZ) * 0.5f)) {
            continue;
        }
        const u32 regionSeed = render::cliffRegionSeed(region);
        for (const render::CliffBand& band : region.cliffBands) {
            // Coarse reject: band bbox vs chunk rect (+ block reach).
            f32 bandMinX = 1.0e9f, bandMaxX = -1.0e9f;
            f32 bandMinZ = 1.0e9f, bandMaxZ = -1.0e9f;
            for (const render::CliffNode& node : band.nodes) {
                bandMinX = glm::min(bandMinX,
                                    glm::min(node.x, node.headX));
                bandMaxX = glm::max(bandMaxX,
                                    glm::max(node.x, node.headX));
                bandMinZ = glm::min(bandMinZ,
                                    glm::min(node.z, node.headZ));
                bandMaxZ = glm::max(bandMaxZ,
                                    glm::max(node.z, node.headZ));
            }
            constexpr f32 kReach = 40.0f;
            if (bandMaxX + kReach < minX || bandMinX - kReach > maxX ||
                bandMaxZ + kReach < minZ || bandMinZ - kReach > maxZ) {
                continue;
            }
            for (const render::CliffBlock& block :
                 render::planCliffBlocks(*params, band, regionSeed)) {
                if (block.base.x < minX || block.base.x >= maxX ||
                    block.base.z < minZ || block.base.z >= maxZ) {
                    continue; // the owning chunk cooks it
                }
                // Same transform as the mesh (world = base +
                // R_y(yaw) R_x(-lean) local, local box x±w/2,
                // y 0..h, z -d..0).
                const glm::quat rot =
                    glm::angleAxis(block.yaw,
                                   Vec3 { 0.0f, 1.0f, 0.0f }) *
                    glm::angleAxis(-block.lean,
                                   Vec3 { 1.0f, 0.0f, 0.0f });
                const Vec3 center =
                    block.base +
                    rot * Vec3 { 0.0f, block.height * 0.5f,
                                 -block.depth * 0.5f };
                const phys::BodyId body = physics.addStaticBox(
                    { block.width * 0.5f, block.height * 0.5f,
                      block.depth * 0.5f },
                    center, rot);
                if (body != 0) {
                    out.push_back(body);
                    ++bodies;
                }
            }
        }
    }
}

void CliffCollision::update(const Vec3& focus) {
    // A terrain republish rebuilds the block set wholesale (the same
    // trigger as the renderer's mesh rebuild).
    const void* key = params->base
                          ? static_cast<const void*>(params->base.get())
                          : nullptr;
    if (key != baseKey) {
        baseKey = key;
        clearAll();
    }
    const f32 chunkSize = render::TerrainSystem::kChunkSize;
    const i32 centerX = static_cast<i32>(std::floor(focus.x / chunkSize));
    const i32 centerZ = static_cast<i32>(std::floor(focus.z / chunkSize));
    i32 bestX = 0;
    i32 bestZ = 0;
    i32 bestDist = INT32_MAX;
    for (i32 tz = centerZ - 1; tz <= centerZ + 1; ++tz) {
        for (i32 tx = centerX - 1; tx <= centerX + 1; ++tx) {
            if (chunks.contains(packChunk(tx, tz))) {
                continue;
            }
            const i32 dist = (tx - centerX) * (tx - centerX) +
                             (tz - centerZ) * (tz - centerZ);
            if (dist < bestDist) {
                bestDist = dist;
                bestX = tx;
                bestZ = tz;
            }
        }
    }
    if (bestDist != INT32_MAX) {
        cookChunk(bestX, bestZ);
    }
    for (auto it = chunks.begin(); it != chunks.end();) {
        const i32 dx = unpackX(it->first) - centerX;
        const i32 dz = unpackZ(it->first) - centerZ;
        if (dx < -2 || dx > 2 || dz < -2 || dz > 2) {
            for (const phys::BodyId body : it->second) {
                physics.removeBody(body);
            }
            bodies -= static_cast<u32>(it->second.size());
            it = chunks.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace game
