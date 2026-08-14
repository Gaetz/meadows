#include "engine/dungeon/DungeonBake.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/core/Rng.hpp"
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

// Walkability as a GENERATION INVARIANT: flood the baked nav grid and
// verify (a) every room center is reachable from the entrance, falls
// allowed (a drop is a legal one-way forward), and (b) the entrance is
// re-reachable CLIMB-ONLY from the goal and from every lever — the two
// geometric stranding families the playtests kept finding one seed at a
// time. A failing embedding is re-rolled before the expensive mesh bake.
bool navValidates(const SpaceGraph& space, const NavGrid& grid,
                  const Vec3& entrancePos,
                  const vector<DungeonBakeResult::Anchor>& levers) {
    if (grid.empty()) {
        return false;
    }
    const f32 maxStep = 0.5f; // slightly under the navigator's own budget
    const auto snap = [&](const Vec3& p) -> i64 {
        const u32 column = grid.columnOf(p.x, p.z);
        if (column == ~0u) {
            return -1;
        }
        u32 begin = 0;
        u32 end = 0;
        grid.columnLevels(column, begin, end);
        i64 best = -1;
        f32 bestDelta = 2.5f;
        for (u32 l = begin; l < end; ++l) {
            const f32 delta = std::abs(grid.levels[l].floorY - p.y);
            if (delta < bestDelta) {
                bestDelta = delta;
                best = l;
            }
        }
        return best;
    };
    const auto flood = [&](i64 start, bool allowFalls) {
        vector<bool> in(grid.levels.size(), false);
        vector<u32> queue;
        if (start < 0) {
            return in;
        }
        in[static_cast<size_t>(start)] = true;
        queue.push_back(static_cast<u32>(start));
        for (size_t head = 0; head < queue.size(); ++head) {
            const u32 level = queue[head];
            // Column coordinates by CSR search.
            u32 column = 0;
            {
                u32 lo = 0;
                u32 hi = grid.width * grid.depth;
                while (lo + 1 < hi) {
                    const u32 mid = (lo + hi) / 2;
                    (grid.firstLevel[mid] <= level ? lo : hi) = mid;
                }
                column = lo;
            }
            const i32 ix = static_cast<i32>(column % grid.width);
            const i32 iz = static_cast<i32>(column / grid.width);
            const f32 y = grid.levels[level].floorY;
            const auto visit = [&](u32 l) {
                if (!in[l]) {
                    in[l] = true;
                    queue.push_back(l);
                }
            };
            if (allowFalls) {
                u32 begin = 0;
                u32 end = 0;
                grid.columnLevels(column, begin, end);
                for (u32 l = begin; l < end; ++l) {
                    if (grid.levels[l].floorY < y - maxStep) {
                        visit(l);
                    }
                }
            }
            const i32 steps[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 },
                                      { 0, -1 } };
            for (const auto& s : steps) {
                const i32 nx = ix + s[0];
                const i32 nz = iz + s[1];
                if (nx < 0 || nz < 0 || nx >= static_cast<i32>(grid.width) ||
                    nz >= static_cast<i32>(grid.depth)) {
                    continue;
                }
                u32 begin = 0;
                u32 end = 0;
                grid.columnLevels(static_cast<u32>(nz) * grid.width +
                                      static_cast<u32>(nx),
                                  begin, end);
                for (u32 l = begin; l < end; ++l) {
                    if (std::abs(grid.levels[l].floorY - y) <= maxStep) {
                        visit(l);
                    }
                }
            }
        }
        return in;
    };

    const i64 entranceLevel = snap(entrancePos);
    if (entranceLevel < 0) {
        return false;
    }
    const vector<bool> forward = flood(entranceLevel, true);
    for (u32 r = 0; r < space.rooms.size(); ++r) {
        const i64 level =
            snap(slotCenter(space.params, space.rooms[r].pos) +
                 Vec3 { 0.0f, 0.3f, 0.0f });
        if (level < 0 || !forward[static_cast<size_t>(level)]) {
            return false;
        }
    }
    const auto climbsHome = [&](const Vec3& from) {
        const i64 level = snap(from + Vec3 { 0.0f, 0.3f, 0.0f });
        return level >= 0 &&
               flood(level, false)[static_cast<size_t>(entranceLevel)];
    };
    if (!climbsHome(slotCenter(space.params,
                               space.rooms[space.goal].pos))) {
        return false;
    }
    for (const auto& lever : levers) {
        if (!climbsHome(lever.position)) {
            return false;
        }
    }
    return true;
}

// The mission semantics made concrete (docs/DUNGEON-GEN.md, populate):
// a barrier across each Locked corridor with its lever in the matching Key
// room, the prize chest at the Goal, ore veins in Reward rooms, enemies on
// Dangerous arcs plus a Goal guardian, one flavor NPC. Floors are flat
// planes at the slot height, so anchors sit at y = floor exactly.
void populateAnchors(DungeonBakeResult& result, const DungeonParams& params,
                     core::Rng& rng) {
    const SpaceGraph& space = result.space;
    const auto roomFloor = [&](const SpaceRoom& room) {
        return slotCenter(space.params, room.pos);
    };
    const auto yawBetween = [](const Vec3& from, const Vec3& to) {
        return glm::degrees(std::atan2(to.x - from.x, to.z - from.z));
    };
    // An anchor must stand where the nav grid baked a floor: corridor
    // wobble and wall noise erode the nominal floor discs, and a prop
    // placed outside them is buried in rock. Y stays the analytic floor
    // plane (exact); the grid is the validity oracle. Actors additionally
    // demand a wall-CLEAR column (the agent-radius story, NavGrid): a
    // walkable cell 10 cm from the rock stands a spawned capsule half
    // inside the wall.
    const auto walkable = [&](const Vec3& p) {
        const u32 column = result.navGrid.columnOf(p.x, p.z);
        if (column == ~0u) {
            return false;
        }
        u32 begin = 0;
        u32 end = 0;
        result.navGrid.columnLevels(column, begin, end);
        for (u32 l = begin; l < end; ++l) {
            if (std::abs(result.navGrid.levels[l].floorY - p.y) <= 0.6f) {
                return true;
            }
        }
        return false;
    };
    const auto standable = [&](const Vec3& p) {
        if (!walkable(p)) {
            return false;
        }
        const u32 column = result.navGrid.columnOf(p.x, p.z);
        const i32 ix = static_cast<i32>(column % result.navGrid.width);
        const i32 iz = static_cast<i32>(column / result.navGrid.width);
        return !result.navGrid.wallAdjacent(ix, iz, p.y);
    };

    // The service exit's lever overrides its Key room: it stands IN the
    // corridor right behind the grille (the Dark Souls shortcut — pull,
    // and you are already at the gate).
    vector<std::pair<u32, Vec3>> leverOverrides;
    for (const SpaceEdge& edge : space.edges) {
        if (edge.kind == EdgeKind::Locked && edge.path.size() >= 3) {
            // The barrier stands on a corridor CELL center: cell centers
            // are pipe junctions the wobble never displaces, so the gate
            // is always inside the carved tube. Prefer a cell with flat
            // hops on both sides (a gate at a ramp mouth gapes over the
            // slope) — nearest the ENTRANCE mouth when the lock touches
            // the entrance (the service exit: the grille shows from the
            // entrance room), nearest the middle otherwise.
            const bool serviceExit =
                edge.a == space.entrance || edge.b == space.entrance;
            const size_t last = edge.path.size() - 2;
            const size_t target =
                !serviceExit               ? edge.path.size() / 2
                : edge.a == space.entrance ? 1
                                           : last;
            const auto straightAt = [&](size_t i) {
                return edge.path[i].x - edge.path[i - 1].x ==
                           edge.path[i + 1].x - edge.path[i].x &&
                       edge.path[i].z - edge.path[i - 1].z ==
                           edge.path[i + 1].z - edge.path[i].z;
            };
            // Nearest flat cell to the target, straight cells beating
            // turns (a gate on a turn must span the junction's diagonal).
            // The service exit keeps distance first — its grille must stay
            // at the entrance mouth; the widened prop covers a diagonal.
            size_t best = target;
            size_t bestDistance = edge.path.size();
            bool bestStraight = false;
            for (size_t i = 1; i <= last; ++i) {
                if (edge.path[i - 1].floor != edge.path[i].floor ||
                    edge.path[i].floor != edge.path[i + 1].floor) {
                    continue;
                }
                const size_t distance =
                    i > target ? i - target : target - i;
                const bool straight = straightAt(i);
                const bool better =
                    serviceExit
                        ? std::pair { distance, !straight } <
                              std::pair { bestDistance, !bestStraight }
                        : std::pair { !straight, distance } <
                              std::pair { !bestStraight, bestDistance };
                if (better) {
                    bestDistance = distance;
                    bestStraight = straight;
                    best = i;
                }
            }
            const Vec3 at = slotCenter(space.params, edge.path[best]);
            result.barriers.push_back(
                { at,
                  yawBetween(slotCenter(space.params, edge.path[best - 1]),
                             slotCenter(space.params, edge.path[best + 1])),
                  edge.lockId, bestStraight ? 1.0f : 1.6f });
            if (serviceExit) {
                // The corridor cell on the NON-entrance side of the grille.
                const size_t leverIdx =
                    edge.a == space.entrance
                        ? std::min(best + 1, last)
                        : (best > 1 ? best - 1 : best);
                leverOverrides.push_back(
                    { edge.lockId,
                      slotCenter(space.params, edge.path[leverIdx]) });
            }
        }
        if (edge.kind == EdgeKind::Dangerous) {
            // Nearest walkable cell center to the corridor's middle.
            const size_t mid = edge.path.size() / 2;
            for (size_t off = 0; off < edge.path.size(); ++off) {
                const size_t i = off % 2 == 0 ? mid + off / 2
                                              : mid - (off + 1) / 2;
                if (i >= edge.path.size()) {
                    continue;
                }
                const Vec3 spot = slotCenter(space.params, edge.path[i]);
                if (standable(spot)) {
                    result.enemySpawns.push_back({ spot, 0.0f, 0 });
                    break;
                }
            }
        }
    }

    // Room degree (space edges touching it): degree-1 rooms are the
    // dead ends the mine dug FOR something.
    vector<u32> degree(space.rooms.size(), 0);
    for (const SpaceEdge& edge : space.edges) {
        ++degree[edge.a];
        ++degree[edge.b];
    }

    const MissionGraph& mission = result.mission;
    for (u32 r = 0; r < space.rooms.size(); ++r) {
        const SpaceRoom& room = space.rooms[r];
        const MissionNode& node = mission.nodes[room.missionNode];
        const Vec3 floor = roomFloor(room);
        // A deterministic scatter offset keeps room props off the exact
        // center (where the player walks in looking); redrawn until the
        // spot passes `fits` — walkable for props (a vein may hug the
        // wall), wall-clear for actors — the validated room center as the
        // fallback.
        const auto scatter = [&](const auto& fits) {
            for (int attempt = 0; attempt < 8; ++attempt) {
                const Vec3 p =
                    floor +
                    Vec3 { static_cast<f32>(rng.unit() - 0.5) * room.radius,
                           0.0f,
                           static_cast<f32>(rng.unit() - 0.5) * room.radius };
                if (fits(p)) {
                    return p;
                }
            }
            return floor;
        };
        if (node.kind == NodeKind::Key) {
            Vec3 leverPos = floor;
            for (const auto& [lockId, pos] : leverOverrides) {
                if (lockId == node.lockId) {
                    leverPos = pos;
                }
            }
            result.levers.push_back({ leverPos, 0.0f, node.lockId });
            continue;
        }
        if (node.kind == NodeKind::Reward) {
            result.oreVeins.push_back({ scatter(walkable), 0.0f, 0 });
            continue;
        }
        if (node.kind != NodeKind::Room) {
            continue;
        }
        result.patrolPoints.push_back({ floor, 0.0f, 0 });
        if (degree[r] <= 1) {
            result.oreVeins.push_back({ scatter(walkable), 0.0f, 0 });
        } else if (rng.chance(params.bonusVeinChancePerRoom)) {
            result.oreVeins.push_back({ scatter(walkable), 0.0f, 0 });
        }
        if (rng.chance(params.enemyChancePerRoom)) {
            result.enemySpawns.push_back(
                { scatter(standable),
                  static_cast<f32>(rng.range(0, 359)), 0 });
        }
    }

    const Vec3 goalFloor =
        roomFloor(space.rooms[space.goal]);
    const Vec3 entranceFloor = roomFloor(space.rooms[space.entrance]);
    result.chests.push_back(
        { goalFloor, yawBetween(goalFloor, entranceFloor), 0 });
    // The guardian stands a step in front of the prize, facing the way in.
    const Vec3 guardPost = goalFloor + Vec3 { 2.0f, 0.0f, 2.0f };
    result.enemySpawns.push_back({ standable(guardPost) ? guardPost
                                                        : goalFloor,
                                   yawBetween(goalFloor, entranceFloor),
                                   0 });
    if (!result.oreVeins.empty()) {
        const Vec3& vein = result.oreVeins.front().position;
        const Vec3 beside = vein + Vec3 { 1.5f, 0.0f, -1.5f };
        result.npcSpawns.push_back(
            { standable(beside) ? beside : vein, 0.0f, 0 });
    }
}

} // namespace

DungeonBakeResult bakeDungeon(const DungeonParams& params) {
    DungeonBakeResult result;

    // One master seed drives every stage through fixed offsets: re-baking
    // with the same params replays the exact dungeon.
    MissionParams mission = params.mission;
    mission.seed = params.seed;
    DensityParams density = params.density;
    density.seed = params.seed ^ 0xD4C3B2A1u;

    result.mission = buildMissionGraph(mission);

    // Nav-validated embedding: carve + nav-bake + anchors are cheap next
    // to the mesh pass, so an embedding whose walkable grid violates the
    // invariant is simply re-rolled with a new space seed (deterministic
    // schedule). Most seeds pass on the first roll.
    bool navOk = false;
    for (u32 navAttempt = 0; navAttempt < 16 && !navOk; ++navAttempt) {
        SpaceParams space = params.space;
        space.seed =
            (params.seed ^ 0x51A9E37Bu) + navAttempt * 0x9E3779B9u;
        result.space = buildSpaceGraph(result.mission, space);
        if (result.space.rooms.empty()) {
            continue;
        }
        const DensityField field(result.space, density);
        result.boundsMin = field.boundsMin();
        result.boundsMax = field.boundsMax();
        result.navGrid = bakeNavGrid(
            [&field](const Vec3& p) { return field.sample(p); },
            field.boundsMin(), field.boundsMax(), params.navCellSize,
            params.navMinClearance);

        const SpaceRoom& entranceRoom =
            result.space.rooms[result.space.entrance];
        result.entranceDir = { -1.0f, 0.0f, 0.0f };
        result.entrancePos =
            slotCenter(result.space.params, entranceRoom.pos) +
            result.entranceDir * (entranceRoom.radius - 1.2f) +
            Vec3 { 0.0f, 0.3f, 0.0f };

        result.barriers.clear();
        result.levers.clear();
        result.chests.clear();
        result.oreVeins.clear();
        result.enemySpawns.clear();
        result.npcSpawns.clear();
        result.patrolPoints.clear();
        core::Rng populateRng(params.seed ^ 0xE11E31E5u);
        populateAnchors(result, params, populateRng);

        navOk = navValidates(result.space, result.navGrid,
                             result.entrancePos, result.levers);
    }
    if (!navOk) {
        return DungeonBakeResult {}; // caller retunes params
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

    result.cellSize = params.cellSize;

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
                // A wobbled tube can leave the nominal centerline in rock:
                // skip that spot rather than bury a torch.
                Vec3 probe = point + Vec3 { 0.0f, 1.8f, 0.0f };
                if (field.sample(probe) >= 0.0f) {
                    continue;
                }
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

    return result;
}

} // namespace dungeon
