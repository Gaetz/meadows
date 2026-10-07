#include "game/scenes/TerrainGenTool.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>

#include <imgui.h>

#include "data/forms/FormQuery.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Paths.hpp"
#include "game/MapBaker.hpp"
#include "game/ui/PropertyGrid.hpp"
#include "world/terrain/MapRecords.hpp"
#include "world/terrain/TerrainRegions.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace game {

namespace {

using render::terraingen::TileBakeParams;
using render::terraingen::TileBakeResult;

} // namespace

void TerrainGenTool::drawPanel(const GenContext& ctx) {
    if (!ImGui::CollapsingHeader("Terrain generation")) {
        return;
    }
    if (ctx.genTuning) {
        drawPupitre(ctx);
    }
    // ---- Bounded-map bake (chantier CARTES M5.3) --------------------
    if (ctx.mapCacheRoot.empty()) {
        return;
    }
    ImGui::SeparatorText("Bounded map");
    if (!mapBake) {
        // Default to the map under the camera.
        const f32 size =
            ctx.mapBakeParams.tileSize *
            static_cast<f32>(kMapTilesPerSide);
        mapX = static_cast<i32>(std::floor(ctx.cameraPos.x / size));
        mapZ = static_cast<i32>(std::floor(ctx.cameraPos.z / size));
    }
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputInt("map X", &mapX);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputInt("map Z", &mapZ);
    const bool mapBaking = mapBake && !mapBake->done.load();
    if (mapBaking) {
        const u32 landed = mapBake->landed.load();
        char label[64];
        std::snprintf(label, sizeof(label), "map (%d, %d): %u/%u slices",
                      mapBake->mapX, mapBake->mapZ, landed,
                      mapBake->total);
        ImGui::ProgressBar(mapBake->total == 0
                               ? 0.0f
                               : static_cast<f32>(landed) /
                                     static_cast<f32>(mapBake->total),
                           ImVec2(-1.0f, 0.0f), label);
        ImGui::TextDisabled("(stage-1 runs before the first slice "
                            "lands — ~2 min total)");
    } else {
        if (ImGui::Button("Bake map")) {
            auto job = std::make_shared<MapBake>();
            job->mapX = mapX;
            job->mapZ = mapZ;
            job->total = static_cast<u32>(kMapTilesPerSide) *
                         static_cast<u32>(kMapTilesPerSide);
            mapBake = job;
            TileBakeParams params = ctx.mapBakeParams;
            const auto work = [params, job, root = ctx.mapCacheRoot,
                               jobsRef = ctx.jobs] {
                if (jobsRef && jobsRef->isStopping()) {
                    job->done.store(true);
                    return;
                }
                const MapBakeStats stats = bakeMap(
                    params, job->mapX, job->mapZ, root, jobsRef,
                    kMapTilesPerSide, [job](u32 landed, u32) {
                        job->landed.store(landed);
                    });
                job->ok.store(stats.slicesWritten == job->total);
                job->done.store(true);
            };
            if (ctx.jobs) {
                ctx.jobs->enqueue(work);
            } else {
                work();
            }
        }
        if (mapBake && mapBake->done.load()) {
            ImGui::SameLine();
            if (!mapBake->ok.load()) {
                ImGui::TextDisabled("map (%d, %d): bake incomplete",
                                    mapBake->mapX, mapBake->mapZ);
            } else if (ImGui::Button("Accept map -> records")) {
                acceptMap(ctx);
            }
        }
        // A map already in the cache can be accepted without rebaking.
        if ((!mapBake || !mapBake->ok.load()) &&
            mapBakedAndValid(ctx.mapCacheRoot, mapX, mapZ,
                             kMapTilesPerSide, &ctx.mapBakeParams)) {
            ImGui::SameLine();
            if (ImGui::Button("Accept cached map -> records")) {
                auto job = std::make_shared<MapBake>();
                job->mapX = mapX;
                job->mapZ = mapZ;
                job->total = static_cast<u32>(kMapTilesPerSide) *
                             static_cast<u32>(kMapTilesPerSide);
                job->ok.store(true);
                job->done.store(true);
                mapBake = job;
                acceptMap(ctx);
            }
        }
    }
}

// The pupitre: every generation knob, live; Apply re-bakes the active
// map (travelToMap on the scene: new streamer, cache miss on the new
// key, background bake behind the veil); Save writes the overlay
// plugin the stack loads at boot; presets are the dev's A/B.
void TerrainGenTool::drawPupitre(const GenContext& ctx) {
    ImGui::SeparatorText("Generation pupitre");
    ImGui::TextDisabled("Edit, then Apply: the active map re-bakes with "
                        "these values (~40 s Release). Save = the "
                        "overlay loaded at boot.");
    if (ImGui::Button("Apply & re-bake map")) {
        if (ctx.applyGenTuning) {
            ctx.applyGenTuning();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save overlay")) {
        if (ctx.saveGenTuning) {
            ctx.saveGenTuning("");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to C++ defaults")) {
        *ctx.genTuning = data::TerrainGenTuningForm {};
    }
    // Presets: named files under data/mods/terrain-gen-presets/.
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("preset name", presetName, sizeof(presetName));
    ImGui::SameLine();
    if (ImGui::Button("Save preset") && presetName[0] != '\0') {
        if (ctx.saveGenTuning) {
            ctx.saveGenTuning(presetName);
        }
    }
    {
        vector<str> presets;
        std::error_code ec;
        if (!ctx.genPresetsDir.empty() &&
            std::filesystem::is_directory(ctx.genPresetsDir, ec)) {
            for (const auto& entry :
                 std::filesystem::directory_iterator(ctx.genPresetsDir, ec)) {
                if (entry.path().extension() == ".toml") {
                    presets.push_back(entry.path().stem().string());
                }
            }
        }
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("##preset", presetPicked.empty()
                                              ? "(preset)"
                                              : presetPicked.c_str())) {
            for (const str& name : presets) {
                if (ImGui::Selectable(name.c_str(), name == presetPicked)) {
                    presetPicked = name;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Load preset") && !presetPicked.empty()) {
            if (ctx.loadGenTuning) {
                ctx.loadGenTuning(presetPicked);
            }
        }
    }
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("filter (world/rhythm/poi/macro/bake)", fieldFilter,
                     sizeof(fieldFilter));
    if (ImGui::BeginChild("pupitre-fields", ImVec2(0.0f, 320.0f),
                          ImGuiChildFlags_Borders)) {
        drawReflectedStruct(ctx.genTuning,
                            data::TerrainGenTuningForm::staticTypeInfo(),
                            fieldFilter);
    }
    ImGui::EndChild();
}

// Accept the baked map: slices copied into the export assets, the
// whole world staged as §5 records through stageMapRecords (the
// ordinary editor Export then ships the mod).
void TerrainGenTool::acceptMap(const GenContext& ctx) {
    if (!mapBake || !mapBake->ok.load()) {
        return;
    }
    const i32 mx = mapBake->mapX;
    const i32 mz = mapBake->mapZ;
    const auto mapDir = mapCacheDir(ctx.mapCacheRoot, mx, mz);

    // Identity: an EXISTING bounded worldspace for these coords keeps
    // its guid (the demo-overworld precedent — patch, never re-mint);
    // a fresh procedural map derives it (§2.5).
    core::Guid mapGuid = world::mapWorldspaceGuid(
        ctx.mapBakeParams.worldSeed, mx, mz);
    str mapName;
    data::forEach<world::WorldspaceForm>(
        ctx.forms, [&](const world::WorldspaceForm& space) {
            if (space.bounded && !space.interior && space.mapX == mx &&
                space.mapZ == mz) {
                mapGuid = space.id;
                mapName = space.editorId;
            }
        });
    if (mapName.empty()) {
        char name[32];
        std::snprintf(name, sizeof(name), "Map_%d_%d", mx, mz);
        mapName = name;
    }

    const auto modsDir = platform::executableDir() / "data" / "mods";
    char rel[64];
    std::snprintf(rel, sizeof(rel), "terrain/map_%d_%d", mx, mz);
    std::error_code errc;
    std::filesystem::create_directories(modsDir / rel, errc);

    vector<world::MapSliceRecord> slices;
    vector<render::terraingen::Lake> lakes;
    vector<render::terraingen::River> rivers;
    for (i32 dz = 0; dz < kMapTilesPerSide; ++dz) {
        for (i32 dx = 0; dx < kMapTilesPerSide; ++dx) {
            const i32 tx = mx * kMapTilesPerSide + dx;
            const i32 tz = mz * kMapTilesPerSide + dz;
            char stem[64];
            std::snprintf(stem, sizeof(stem), "tile_%d_%d_v%u", tx, tz,
                          render::terraingen::kTileBakeVersion);
            const auto dest = modsDir / rel / (str { stem } + ".trg");
            std::filesystem::copy_file(
                mapDir / (str { stem } + ".trg"), dest,
                std::filesystem::copy_options::overwrite_existing,
                errc);
            if (errc) {
                LOG_ERROR("Map accept: cannot copy slice ({}, {})", tx,
                          tz);
                return;
            }
            const u32 index =
                static_cast<u32>(dz * kMapTilesPerSide + dx);
            const core::Guid asset =
                world::mapSliceAssetGuid(mapGuid, index);
            ctx.levelEditor.addExportAsset(
                asset, str { rel } + "/" + stem + ".trg");
            slices.push_back(
                { tx, tz, asset,
                  render::terraingen::kRegionDetailAmplitude,
                  render::terraingen::kRegionDetailWavelength,
                  render::terraingen::kRegionDetailOctaves });
            vector<render::terraingen::Lake> sliceLakes;
            vector<render::terraingen::River> sliceRivers;
            if (readWaterFile(mapDir / (str { stem } + ".twb"),
                              sliceLakes, sliceRivers)) {
                for (auto& lake : sliceLakes) {
                    lakes.push_back(std::move(lake));
                }
                for (auto& river : sliceRivers) {
                    rivers.push_back(std::move(river));
                }
            }
        }
    }

    world::stageMapRecords(
        ctx.levelEditor.editSession(), ctx.forms, mapGuid, mapName, mx,
        mz,
        ctx.mapBakeParams.tileSize * static_cast<f32>(kMapTilesPerSide),
        ctx.mapBakeParams.worldSeed, slices, lakes, rivers, 48.0f);
}

} // namespace game
