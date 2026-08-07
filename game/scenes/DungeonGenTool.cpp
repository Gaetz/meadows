#include "game/scenes/DungeonGenTool.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>

#include <imgui.h>

#include "engine/assets/CookedMesh.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Paths.hpp"
#include "world/dungeon/DungeonRecords.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace game {

namespace {

// The generated-records guid family, next slot after terrain's 7e88a111:
// one dungeon per seed, stable across re-Accepts (re-bake = patch).
core::Guid dungeonGuidFor(u32 seed) {
    char text[40];
    std::snprintf(text, sizeof(text), "7e88a112-0000-4000-8000-%012llx",
                  static_cast<unsigned long long>(seed));
    return *core::Guid::fromString(text);
}

core::Guid cellMeshAssetGuid(const core::Guid& dungeonId, i32 cx, i32 cz) {
    const u64 key = (static_cast<u64>(static_cast<u32>(cx)) << 32) |
                    static_cast<u64>(static_cast<u32>(cz));
    return core::Guid::combine(dungeonId,
                               core::Guid { key, 0x636D657368617373ull });
}

core::Guid navAssetGuid(const core::Guid& dungeonId) {
    return core::Guid::combine(dungeonId,
                               core::Guid { 1, 0x6E76676173736574ull });
}

} // namespace

void DungeonGenTool::drawPanel(const DungeonGenContext& ctx) {
    if (!ImGui::CollapsingHeader("Dungeon generation")) {
        return;
    }
    if (!seedInit) {
        seed = ctx.defaultSeed;
        seedInit = true;
    }
    dungeon::DungeonBakeResult baked;
    while (done->tryPop(baked)) {
        baking = false;
        result = std::move(baked);
    }

    int seedInt = static_cast<int>(seed);
    if (ImGui::InputInt("Dungeon seed", &seedInt)) {
        seed = static_cast<u32>(seedInt);
    }
    ImGui::SliderInt("Floors", &floors, 1, 4);
    ImGui::SliderInt("Grid", &gridXZ, 4, 9);
    ImGui::SliderInt("Side cycles", &subCycles, 0, 4);
    if (baking) {
        ImGui::TextDisabled("Baking (D1..D6 on a worker)...");
    } else if (ImGui::Button("Bake mine")) {
        baking = true;
        result.reset();
        dungeon::DungeonParams params;
        params.seed = seed;
        params.mission.subCycles = subCycles;
        params.space.floors = floors;
        params.space.gridX = gridXZ;
        params.space.gridZ = gridXZ;
        const auto work = [params, queue = done] {
            queue->push(dungeon::bakeDungeon(params));
        };
        if (ctx.jobs) {
            ctx.jobs->enqueue(work);
        } else {
            work();
        }
    }
    if (result) {
        if (result->empty()) {
            ImGui::TextColored({ 1.0f, 0.5f, 0.4f, 1.0f },
                               "Bake failed to embed: retune (bigger grid?)");
        } else {
            u32 triangles = 0;
            for (const auto& cellMesh : result->cellMeshes) {
                triangles +=
                    static_cast<u32>(cellMesh.mesh.indices.size()) / 3;
            }
            ImGui::Text("%zu rooms, %zu cells, %zu torches, %u tris",
                        result->space.rooms.size(),
                        result->cellMeshes.size(), result->torches.size(),
                        triangles);
            if (ImGui::Button("Accept -> records (door here)")) {
                accept(ctx);
            }
        }
    }
    ImGui::TextDisabled("Output = ordinary records; retouch with the level "
                        "editor, Export ships the mod.");
}

void DungeonGenTool::accept(const DungeonGenContext& ctx) {
    if (!result || result->empty()) {
        return;
    }
    const auto* activeWs = static_cast<const world::WorldspaceForm*>(
        ctx.forms.get(ctx.activeWorldspace));
    if (!activeWs) {
        LOG_ERROR("Dungeon gen: no active worldspace for the door");
        return;
    }
    const core::Guid dungeonId = dungeonGuidFor(seed);

    // Assets on disk, registered LIVE (travel this session) and for export.
    char dirName[64];
    std::snprintf(dirName, sizeof(dirName), "dungeon_%u", seed);
    const auto modsDir = platform::executableDir() / "data" / "mods";
    const auto dir = modsDir / "dungeons" / dirName;
    std::error_code errc;
    std::filesystem::create_directories(dir, errc);

    for (const auto& cellMesh : result->cellMeshes) {
        char name[64];
        std::snprintf(name, sizeof(name), "cell_%d_%d.cmesh", cellMesh.cx,
                      cellMesh.cz);
        if (!assets::saveCookedMesh(dir / name, cellMesh.mesh,
                                    dungeon::kDungeonBakeVersion)) {
            return;
        }
        const core::Guid asset =
            cellMeshAssetGuid(dungeonId, cellMesh.cx, cellMesh.cz);
        const str relative =
            str { "dungeons/" } + dirName + "/" + name;
        ctx.assetDb.add(asset, modsDir, relative);
        ctx.levelEditor.addExportAsset(asset, relative);
    }
    if (!dungeon::writeNvgFile(dir / "nav.nvg", result->navGrid,
                               dungeon::kDungeonBakeVersion)) {
        return;
    }
    const core::Guid navAsset = navAssetGuid(dungeonId);
    const str navRelative = str { "dungeons/" } + dirName + "/nav.nvg";
    ctx.assetDb.add(navAsset, modsDir, navRelative);
    ctx.levelEditor.addExportAsset(navAsset, navRelative);

    // The outside door: at the camera, in the active worldspace's cell
    // (materialized live + shipped, the ensureCell pattern).
    const f32 cellSize = activeWs->cellSize;
    const i32 gx = static_cast<i32>(std::floor(ctx.cameraPos.x / cellSize));
    const i32 gz = static_cast<i32>(std::floor(ctx.cameraPos.z / cellSize));
    world::DungeonAnchor anchor;
    anchor.cell = ctx.levelEditor.ensureCell(ctx.worldModel, ctx.forms,
                                             ctx.activeWorldspace, gx, gz);
    anchor.doorPos = ctx.cameraPos;
    anchor.yawDeg = ctx.cameraYawDeg;

    char dungeonName[64];
    std::snprintf(dungeonName, sizeof(dungeonName), "Mine_%u", seed);
    const world::DungeonStageResult staged = world::stageDungeonRecords(
        ctx.levelEditor.editSession(), ctx.forms, ctx.worldModel, *result,
        dungeonId, dungeonName,
        [&](i32 cx, i32 cz) { return cellMeshAssetGuid(dungeonId, cx, cz); },
        navAsset, { anchor }, ctx.doorModel, ctx.doorMaterial);
    LOG_INFO("Dungeon gen: '{}' staged ({} cells, {} torches) — door at "
             "({:.1f}, {:.1f}, {:.1f}); Export writes the mod",
             dungeonName, staged.cellCount, staged.torchCount,
             ctx.cameraPos.x, ctx.cameraPos.y, ctx.cameraPos.z);
}

} // namespace game
