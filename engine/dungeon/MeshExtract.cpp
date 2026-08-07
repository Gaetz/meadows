#include "engine/dungeon/MeshExtract.hpp"

#include <cmath>
#include <unordered_map>

namespace dungeon {

namespace {

struct CellKey {
    i64 packed;
    bool operator==(const CellKey&) const = default;
};

struct CellKeyHash {
    size_t operator()(const CellKey& k) const {
        return std::hash<i64>()(k.packed);
    }
};

i64 pack(i32 x, i32 y, i32 z) {
    // 21 bits per axis, biased: plenty for world-sized lattices.
    const i64 bias = 1 << 20;
    return ((static_cast<i64>(x) + bias) << 42) |
           ((static_cast<i64>(y) + bias) << 21) |
           (static_cast<i64>(z) + bias);
}

} // namespace

render::MeshData extractChunkMesh(const DensityFn& density,
                                  const Vec3& chunkMin, const Vec3& chunkMax,
                                  f32 voxelSize) {
    render::MeshData mesh;

    // Global lattice bounds covering the chunk plus a one-cell apron (cells
    // adjacent to an owned edge may live just outside the chunk).
    const i32 x0 = static_cast<i32>(std::floor(chunkMin.x / voxelSize)) - 1;
    const i32 y0 = static_cast<i32>(std::floor(chunkMin.y / voxelSize)) - 1;
    const i32 z0 = static_cast<i32>(std::floor(chunkMin.z / voxelSize)) - 1;
    const i32 x1 = static_cast<i32>(std::ceil(chunkMax.x / voxelSize)) + 1;
    const i32 y1 = static_cast<i32>(std::ceil(chunkMax.y / voxelSize)) + 1;
    const i32 z1 = static_cast<i32>(std::ceil(chunkMax.z / voxelSize)) + 1;
    const i32 nx = x1 - x0 + 1;
    const i32 ny = y1 - y0 + 1;
    const i32 nz = z1 - z0 + 1;

    const auto latticePos = [&](i32 gx, i32 gy, i32 gz) {
        return Vec3 { static_cast<f32>(gx) * voxelSize,
                      static_cast<f32>(gy) * voxelSize,
                      static_cast<f32>(gz) * voxelSize };
    };

    vector<f32> samples(static_cast<size_t>(nx) * ny * nz);
    const auto sampleAt = [&](i32 gx, i32 gy, i32 gz) -> f32& {
        return samples[(static_cast<size_t>(gz - z0) * ny +
                        static_cast<size_t>(gy - y0)) *
                           nx +
                       static_cast<size_t>(gx - x0)];
    };
    for (i32 gz = z0; gz <= z1; ++gz) {
        for (i32 gy = y0; gy <= y1; ++gy) {
            for (i32 gx = x0; gx <= x1; ++gx) {
                sampleAt(gx, gy, gz) = density(latticePos(gx, gy, gz));
            }
        }
    }

    // One vertex per cell with a sign change: centroid of the zero crossings
    // of its 12 edges. Keyed by GLOBAL cell coordinates so both sides of a
    // chunk seam derive the exact same position.
    std::unordered_map<CellKey, u32, CellKeyHash> cellVertex;
    const auto vertexOf = [&](i32 cx, i32 cy, i32 cz) -> i64 {
        const CellKey key { pack(cx, cy, cz) };
        const auto it = cellVertex.find(key);
        if (it != cellVertex.end()) {
            return it->second;
        }
        if (cx < x0 || cy < y0 || cz < z0 || cx + 1 > x1 || cy + 1 > y1 ||
            cz + 1 > z1) {
            return -1;
        }
        f32 corner[2][2][2];
        for (i32 dz = 0; dz <= 1; ++dz) {
            for (i32 dy = 0; dy <= 1; ++dy) {
                for (i32 dx = 0; dx <= 1; ++dx) {
                    corner[dx][dy][dz] =
                        sampleAt(cx + dx, cy + dy, cz + dz);
                }
            }
        }
        Vec3 sum { 0.0f };
        i32 crossings = 0;
        const auto edge = [&](i32 ax, i32 ay, i32 az, i32 bx, i32 by,
                              i32 bz) {
            const f32 a = corner[ax][ay][az];
            const f32 b = corner[bx][by][bz];
            if ((a < 0.0f) == (b < 0.0f)) {
                return;
            }
            const f32 t = a / (a - b);
            const Vec3 pa = latticePos(cx + ax, cy + ay, cz + az);
            const Vec3 pb = latticePos(cx + bx, cy + by, cz + bz);
            sum += pa + (pb - pa) * t;
            ++crossings;
        };
        edge(0, 0, 0, 1, 0, 0); edge(0, 1, 0, 1, 1, 0);
        edge(0, 0, 1, 1, 0, 1); edge(0, 1, 1, 1, 1, 1);
        edge(0, 0, 0, 0, 1, 0); edge(1, 0, 0, 1, 1, 0);
        edge(0, 0, 1, 0, 1, 1); edge(1, 0, 1, 1, 1, 1);
        edge(0, 0, 0, 0, 0, 1); edge(1, 0, 0, 1, 0, 1);
        edge(0, 1, 0, 0, 1, 1); edge(1, 1, 0, 1, 1, 1);
        if (crossings == 0) {
            return -1;
        }
        const Vec3 pos = sum / static_cast<f32>(crossings);

        render::MeshVertex v;
        v.position = pos;
        const f32 e = voxelSize * 0.5f;
        const Vec3 grad {
            density({ pos.x + e, pos.y, pos.z }) -
                density({ pos.x - e, pos.y, pos.z }),
            density({ pos.x, pos.y + e, pos.z }) -
                density({ pos.x, pos.y - e, pos.z }),
            density({ pos.x, pos.y, pos.z + e }) -
                density({ pos.x, pos.y, pos.z - e }),
        };
        const f32 len = glm::length(grad);
        // Density grows toward rock; the render normal faces the air.
        v.normal = len > 0.0001f ? -grad / len : Vec3 { 0.0f, 1.0f, 0.0f };
        v.uv = { pos.x * 0.25f, pos.z * 0.25f };
        v.color = { 1.0f, 1.0f, 1.0f };
        const u32 index = static_cast<u32>(mesh.vertices.size());
        mesh.vertices.push_back(v);
        cellVertex.emplace(key, index);
        return index;
    };

    // A quad per crossed lattice edge, connecting the vertices of the four
    // cells around it. Edge ownership by midpoint keeps every quad emitted by
    // exactly one chunk.
    const auto owns = [&](const Vec3& p) {
        return p.x >= chunkMin.x && p.x < chunkMax.x && p.y >= chunkMin.y &&
               p.y < chunkMax.y && p.z >= chunkMin.z && p.z < chunkMax.z;
    };
    const auto emitQuad = [&](i64 v0, i64 v1, i64 v2, i64 v3, bool flip) {
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) {
            return;
        }
        const u32 a = static_cast<u32>(v0);
        const u32 b = static_cast<u32>(flip ? v3 : v1);
        const u32 c = static_cast<u32>(v2);
        const u32 d = static_cast<u32>(flip ? v1 : v3);
        mesh.indices.insert(mesh.indices.end(), { a, b, c, a, c, d });
    };

    for (i32 gz = z0; gz < z1; ++gz) {
        for (i32 gy = y0; gy < y1; ++gy) {
            for (i32 gx = x0; gx < x1; ++gx) {
                const f32 here = sampleAt(gx, gy, gz);
                // X-directed edge.
                if (gx + 1 <= x1) {
                    const f32 there = sampleAt(gx + 1, gy, gz);
                    if ((here < 0.0f) != (there < 0.0f)) {
                        const Vec3 mid =
                            latticePos(gx, gy, gz) +
                            Vec3 { voxelSize * 0.5f, 0.0f, 0.0f };
                        if (owns(mid)) {
                            emitQuad(vertexOf(gx, gy - 1, gz - 1),
                                     vertexOf(gx, gy, gz - 1),
                                     vertexOf(gx, gy, gz),
                                     vertexOf(gx, gy - 1, gz),
                                     here < 0.0f);
                        }
                    }
                }
                // Y-directed edge.
                if (gy + 1 <= y1) {
                    const f32 there = sampleAt(gx, gy + 1, gz);
                    if ((here < 0.0f) != (there < 0.0f)) {
                        const Vec3 mid =
                            latticePos(gx, gy, gz) +
                            Vec3 { 0.0f, voxelSize * 0.5f, 0.0f };
                        if (owns(mid)) {
                            emitQuad(vertexOf(gx - 1, gy, gz - 1),
                                     vertexOf(gx, gy, gz - 1),
                                     vertexOf(gx, gy, gz),
                                     vertexOf(gx - 1, gy, gz),
                                     there < 0.0f);
                        }
                    }
                }
                // Z-directed edge.
                if (gz + 1 <= z1) {
                    const f32 there = sampleAt(gx, gy, gz + 1);
                    if ((here < 0.0f) != (there < 0.0f)) {
                        const Vec3 mid =
                            latticePos(gx, gy, gz) +
                            Vec3 { 0.0f, 0.0f, voxelSize * 0.5f };
                        if (owns(mid)) {
                            emitQuad(vertexOf(gx - 1, gy - 1, gz),
                                     vertexOf(gx, gy - 1, gz),
                                     vertexOf(gx, gy, gz),
                                     vertexOf(gx - 1, gy, gz),
                                     here < 0.0f);
                        }
                    }
                }
            }
        }
    }
    return mesh;
}

} // namespace dungeon
