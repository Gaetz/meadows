#include "engine/dungeon/DungeonBake.hpp"

#include <cmath>

#include "engine/dungeon/MeshExtract.hpp"
#include "engine/terrain/Noise.hpp"

namespace dungeon {

namespace {

// Fixed probe directions (hemisphere when mirrored onto a normal): a small
// golden-spiral set, the same for every vertex so bakes replay bit for bit.
vector<Vec3> probeDirections() {
    vector<Vec3> dirs;
    const i32 count = 14;
    const f32 golden = 2.399963f;
    for (i32 i = 0; i < count; ++i) {
        const f32 t = (static_cast<f32>(i) + 0.5f) / count;
        const f32 y = 1.0f - 2.0f * t;
        const f32 r = std::sqrt(glm::max(0.0f, 1.0f - y * y));
        const f32 a = golden * static_cast<f32>(i);
        dirs.push_back({ r * std::cos(a), y, r * std::sin(a) });
    }
    return dirs;
}

// AO in [0, 1] (1 = open) by probing the field itself around the vertex.
f32 fieldOcclusion(const DensityField& field, const Vec3& pos,
                   const Vec3& normal, const vector<Vec3>& dirs) {
    i32 open = 0;
    i32 total = 0;
    for (const Vec3& raw : dirs) {
        const Vec3 dir = glm::dot(raw, normal) < 0.0f ? -raw : raw;
        for (const f32 radius : { 0.7f, 1.6f }) {
            const Vec3 probe = pos + normal * 0.25f + dir * radius;
            ++total;
            if (field.sample(probe) < 0.0f) {
                ++open;
            }
        }
    }
    return static_cast<f32>(open) / static_cast<f32>(total);
}

void bakeVertexColors(render::MeshData& mesh, const DensityField& field,
                      u32 seed, const vector<Vec3>& dirs) {
    const Vec3 baseRock { 0.44f, 0.40f, 0.36f };
    const Vec3 paleRock { 0.55f, 0.52f, 0.46f };
    for (render::MeshVertex& v : mesh.vertices) {
        const f32 strata = render::noise::fbm3(seed ^ 0xC0104u,
                                               v.position * 0.12f, 1.0f, 2,
                                               2.0f, 0.5f);
        const f32 ao = fieldOcclusion(field, v.position, v.normal, dirs);
        const Vec3 tint = glm::mix(baseRock, paleRock, strata);
        v.color = tint * (0.35f + 0.65f * ao);
    }
}

} // namespace

DungeonBakeResult bakeDungeon(const DungeonParams& params) {
    DungeonBakeResult result;

    // One master seed drives every stage through fixed offsets: re-baking
    // with the same params replays the exact dungeon.
    MissionParams mission = params.mission;
    mission.seed = params.seed;
    SpaceParams space = params.space;
    space.seed = params.seed ^ 0x51A9E37Bu;
    DensityParams density = params.density;
    density.seed = params.seed ^ 0xD4C3B2A1u;

    result.mission = buildMissionGraph(mission);
    result.space = buildSpaceGraph(result.mission, space);
    if (result.space.rooms.empty()) {
        return result; // could not embed: caller retunes params
    }

    const DensityField field(result.space, density);
    const DensityFn densityFn = [&field](const Vec3& p) {
        return field.sample(p);
    };
    const vector<Vec3> dirs = probeDirections();

    // Chunk the carved bounds by world cell; vertical extent rides in one
    // chunk per cell (the interior streamer loads whole XZ columns anyway).
    const f32 cell = params.cellSize;
    const i32 cx0 = static_cast<i32>(std::floor(field.boundsMin().x / cell));
    const i32 cz0 = static_cast<i32>(std::floor(field.boundsMin().z / cell));
    const i32 cx1 = static_cast<i32>(std::floor(field.boundsMax().x / cell));
    const i32 cz1 = static_cast<i32>(std::floor(field.boundsMax().z / cell));
    for (i32 cz = cz0; cz <= cz1; ++cz) {
        for (i32 cx = cx0; cx <= cx1; ++cx) {
            const Vec3 lo { static_cast<f32>(cx) * cell,
                            field.boundsMin().y - 2.0f,
                            static_cast<f32>(cz) * cell };
            const Vec3 hi { static_cast<f32>(cx + 1) * cell,
                            field.boundsMax().y + 2.0f,
                            static_cast<f32>(cz + 1) * cell };
            render::MeshData mesh =
                extractChunkMesh(densityFn, lo, hi, params.voxelSize);
            if (mesh.vertices.empty()) {
                continue;
            }
            bakeVertexColors(mesh, field, params.seed, dirs);
            // Re-base on the cell corner: the emitted reference carries the
            // corner as its position, so the mesh loads as an ordinary
            // static prop.
            const Vec3 corner { static_cast<f32>(cx) * cell, 0.0f,
                                static_cast<f32>(cz) * cell };
            for (render::MeshVertex& v : mesh.vertices) {
                v.position -= corner;
            }
            result.cellMeshes.push_back({ cx, cz, std::move(mesh) });
        }
    }

    result.boundsMin = field.boundsMin();
    result.boundsMax = field.boundsMax();
    result.cellSize = params.cellSize;
    result.navGrid =
        bakeNavGrid(densityFn, field.boundsMin(), field.boundsMax(),
                    params.navCellSize, params.navMinClearance);

    // Torch anchors along visible corridors: march the centerline, drop an
    // anchor on alternating walls every torchSpacing meters. Hidden passages
    // stay dark on purpose.
    u32 torchIndex = 0;
    for (const SpaceEdge& edge : result.space.edges) {
        if (edge.kind == EdgeKind::Hidden) {
            continue;
        }
        f32 distanceLeft = params.torchSpacing * 0.5f;
        for (size_t i = 1; i < edge.path.size(); ++i) {
            const Vec3 a = slotCenter(result.space.params, edge.path[i - 1]);
            const Vec3 b = slotCenter(result.space.params, edge.path[i]);
            const Vec3 seg = b - a;
            const f32 len = glm::length(seg);
            if (len < 0.001f) {
                continue;
            }
            const Vec3 dir = seg / len;
            if (std::abs(dir.y) > 0.7f) {
                continue; // no torches down a shaft
            }
            f32 travelled = 0.0f;
            while (travelled + distanceLeft < len) {
                travelled += distanceLeft;
                distanceLeft = params.torchSpacing;
                const Vec3 point = a + dir * travelled;
                const Vec3 side = glm::normalize(
                    glm::cross(dir, Vec3 { 0.0f, 1.0f, 0.0f }));
                const Vec3 wallDir = (torchIndex++ % 2 == 0) ? side : -side;
                // Probe from torch height out to the wall, then back off.
                Vec3 probe = point + Vec3 { 0.0f, 1.8f, 0.0f };
                f32 out = 0.0f;
                while (field.sample(probe + wallDir * out) < 0.0f &&
                       out < 8.0f) {
                    out += 0.2f;
                }
                if (out >= 8.0f) {
                    continue; // open cavern side: no wall to hang from
                }
                result.torches.push_back(
                    { probe + wallDir * (out - 0.4f), -wallDir });
            }
            distanceLeft -= len - travelled;
        }
    }

    const SpaceRoom& entrance = result.space.rooms[result.space.entrance];
    result.entrancePos = slotCenter(result.space.params, entrance.pos) +
                         Vec3 { 0.0f, 0.3f, 0.0f };
    result.entranceDir = { -1.0f, 0.0f, 0.0f }; // the entrance hugs x = 0
    return result;
}

} // namespace dungeon
