#include "game/scenes/DungeonGenTool.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>

#include <imgui.h>

#include "data/forms/CoreForms.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/VisualForms.hpp"
#include "engine/assets/CookedMesh.hpp"
#include "gameplay/interaction/FurnitureForms.hpp"
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
    ImGui::SliderInt("Grid (size)", &gridXZ, 6, 12);
    ImGui::SliderInt("Arc rooms (length)", &arcRooms, 1, 4);
    ImGui::SliderInt("Side cycles (complexity)", &subCycles, 0, 4);
    if (baking) {
        ImGui::TextDisabled("Baking (D1..D6 on a worker)...");
    } else if (ImGui::Button("Bake mine")) {
        baking = true;
        result.reset();
        dungeon::DungeonParams params;
        params.seed = seed;
        params.mission.subCycles = subCycles;
        params.mission.arcRoomsMax = arcRooms;
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
    // Records for this dungeon already RESOLVED (a mod exported by an
    // earlier session): staging updates them in place, so this session
    // plays THIS bake — but the mod on disk still carries the old one.
    if (ctx.forms.find(dungeonId) && !acceptedSeeds.contains(seed)) {
        LOG_WARN("Dungeon gen: seed {} was exported by a previous session — "
                 "its records are updated live to this bake. Export again "
                 "or the next session reloads the old version.",
                 seed);
    }
    // Assets: write EVERY file first, register nothing until all of them
    // landed — a failed write must leave no half-registered state (the
    // seed stays un-accepted, so Accept can simply be retried; orphan
    // files from the failed run are rewritten then).
    char dirName[64];
    std::snprintf(dirName, sizeof(dirName), "dungeon_%u", seed);
    const auto modsDir = platform::executableDir() / "data" / "mods";
    const auto dir = modsDir / "dungeons" / dirName;
    std::error_code errc;
    std::filesystem::create_directories(dir, errc);

    vector<std::pair<core::Guid, str>> writtenAssets;
    writtenAssets.reserve(result->cellMeshes.size() + 1);
    for (const auto& cellMesh : result->cellMeshes) {
        char name[64];
        std::snprintf(name, sizeof(name), "cell_%d_%d.cmesh", cellMesh.cx,
                      cellMesh.cz);
        if (!assets::saveCookedMesh(dir / name, cellMesh.mesh,
                                    dungeon::kDungeonBakeVersion)) {
            LOG_ERROR("Dungeon gen: writing {} failed — accept aborted, "
                      "nothing registered (retry Accept)",
                      (dir / name).string());
            return;
        }
        writtenAssets.emplace_back(
            cellMeshAssetGuid(dungeonId, cellMesh.cx, cellMesh.cz),
            str { "dungeons/" } + dirName + "/" + name);
    }
    if (!dungeon::writeNvgFile(dir / "nav.nvg", result->navGrid,
                               dungeon::kDungeonBakeVersion)) {
        LOG_ERROR("Dungeon gen: writing {} failed — accept aborted, "
                  "nothing registered (retry Accept)",
                  (dir / "nav.nvg").string());
        return;
    }
    writtenAssets.emplace_back(navAssetGuid(dungeonId),
                               str { "dungeons/" } + dirName + "/nav.nvg");

    acceptedSeeds.insert(seed);
    for (const auto& [asset, relative] : writtenAssets) {
        ctx.assetDb.add(asset, modsDir, relative);
        ctx.levelEditor.addExportAsset(asset, relative);
    }
    const core::Guid navAsset = navAssetGuid(dungeonId);

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

    // Dress the doors with whatever door leaf the data already ships
    // (first DoorForm with a model): no asset guid hardcoded, and a
    // data-less session degrades to the placeholder box.
    core::Guid doorModel;
    core::Guid doorMaterial;
    data::forEach<world::DoorForm>(ctx.forms, [&](const world::DoorForm& d) {
        if (!doorModel.isValid() && d.model.isValid()) {
            doorModel = d.model;
            doorMaterial = d.material;
        }
    });

    // The mine kit (mine-kit.toml), resolved by editorId: gameplay anchors
    // become real records. Any missing form just skips its family.
    world::DungeonKit kit;
    const auto guidOf = [](const auto* form) {
        return form ? form->id : core::Guid {};
    };
    kit.barrier = guidOf(
        data::findByEditorId<data::StaticForm>(ctx.forms, "MineBarrier"));
    kit.lever = guidOf(data::findByEditorId<gameplay::FurnitureForm>(
        ctx.forms, "MineLever"));
    kit.chest = guidOf(data::findByEditorId<gameplay::FurnitureForm>(
        ctx.forms, "MineChest"));
    kit.oreItem = guidOf(
        data::findByEditorId<data::MiscItemForm>(ctx.forms, "OreChunk"));
    kit.enemy = guidOf(
        data::findByEditorId<data::ActorForm>(ctx.forms, "Bandit"));
    kit.npc = guidOf(
        data::findByEditorId<data::ActorForm>(ctx.forms, "MineHermit"));
    if (!kit.barrier.isValid() || !kit.lever.isValid() ||
        !kit.chest.isValid()) {
        LOG_WARN("Dungeon gen: mine-kit forms missing (is mine-kit.toml in "
                 "the plugin stack of this session?) — the affected "
                 "families will not be staged");
    }

    char dungeonName[64];
    std::snprintf(dungeonName, sizeof(dungeonName), "Mine_%u", seed);
    const world::DungeonStageResult staged = world::stageDungeonRecords(
        ctx.levelEditor.editSession(), ctx.forms, ctx.worldModel, *result,
        dungeonId, dungeonName,
        [&](i32 cx, i32 cz) { return cellMeshAssetGuid(dungeonId, cx, cz); },
        navAsset, { anchor }, kit, doorModel, doorMaterial);
    LOG_INFO("Dungeon gen: '{}' staged ({} cells, {} torches) — door at "
             "({:.1f}, {:.1f}, {:.1f}); Export writes the mod",
             dungeonName, staged.cellCount, staged.torchCount,
             ctx.cameraPos.x, ctx.cameraPos.y, ctx.cameraPos.z);
}

} // namespace game
