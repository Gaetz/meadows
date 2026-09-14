#include "game/scenes/TerrainGenTool.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>

#include <imgui.h>

#include "data/forms/FormQuery.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Paths.hpp"
#include "game/MapBaker.hpp"
#include "world/terrain/MapRecords.hpp"
#include "world/terrain/TerrainRegions.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace game {

namespace {

using render::terraingen::TileBakeParams;
using render::terraingen::TileBakeResult;

// Deterministic guid families for generated records (the sculpt tool's
// 7e88a110 scheme, next slots): stable across re-Accepts so a re-bake
// PATCHES the same records instead of duplicating them.
core::Guid regionAssetGuid(i32 tx, i32 tz) {
    const u64 key = (static_cast<u64>(static_cast<u32>(tx)) << 32) |
                    static_cast<u64>(static_cast<u32>(tz));
    char text[40];
    std::snprintf(text, sizeof(text), "7e88a111-0000-4000-8000-%012llx",
                  static_cast<unsigned long long>(key & 0xFFFFFFFFFFFFull));
    return *core::Guid::fromString(text);
}

core::Guid derivedGuid(const core::Guid& family, u64 index) {
    return core::Guid::combine(family,
                               core::Guid { index, 0x7465727261696e67ull });
}

} // namespace

void TerrainGenTool::drawPanel(const GenContext& ctx) {
    if (!ImGui::CollapsingHeader("Terrain generation")) {
        return;
    }
    if (!seedInit) {
        seed = ctx.defaultSeed;
        seedInit = true;
    }
    // Land finished bakes (the streamer's worker mailbox, drained on
    // the frame thread).
    if (streamer) {
        streamer->drain(
            [&](TerrainBakeStreamer::PublishedTile&& tile) {
                baking = false;
                tileX = tile.tx;
                tileZ = tile.tz;
                TileBakeResult landed;
                landed.region = std::move(tile.region);
                landed.lakes = std::move(tile.lakes);
                landed.rivers = std::move(tile.rivers);
                result = std::move(landed);
                if (ctx.publishPreview) {
                    TileBakeResult copy = *result;
                    ctx.publishPreview(std::move(copy), tileX, tileZ);
                }
            });
    }

    int seedInt = static_cast<int>(seed);
    if (ImGui::InputInt("Gen seed", &seedInt)) {
        seed = static_cast<u32>(seedInt);
    }
    const char* sizes[] = { "1 km", "2 km", "4 km" };
    int sizeIndex = regionSize > 3000.0f ? 2
                    : regionSize > 1500.0f ? 1
                                           : 0;
    if (ImGui::Combo("Region size", &sizeIndex, sizes, 3)) {
        regionSize = sizeIndex == 2 ? 4096.0f
                     : sizeIndex == 1 ? 2048.0f
                                      : 1024.0f;
    }
    if (baking) {
        ImGui::TextDisabled("Baking (S1..S6 on a worker)...");
    } else if (ImGui::Button("Bake region here")) {
        baking = true;
        result.reset();
        tileX = static_cast<i32>(std::floor(ctx.cameraPos.x / regionSize));
        tileZ = static_cast<i32>(std::floor(ctx.cameraPos.z / regionSize));
        // The streamer's cache directory keys what its filenames do not
        // (seed + tile size); a seed/size change gets a fresh streamer,
        // in-flight bakes of the old one land in the void harmlessly
        // (shared-queue teardown contract).
        if (!streamer || streamerSeed != seed ||
            streamerSize != regionSize) {
            TileBakeParams params;
            params.worldSeed = seed;
            params.tileSize = regionSize;
            char dir[48];
            std::snprintf(dir, sizeof(dir), "gen_%u_%d", seed,
                          static_cast<i32>(regionSize));
            streamer = std::make_unique<TerrainBakeStreamer>(
                params, platform::executableDir() / "terrain-cache" / dir,
                ctx.jobs);
            streamerSeed = seed;
            streamerSize = regionSize;
        }
        // Re-baking a visited tile must re-run the request (a cache
        // read), not be skipped as already published.
        streamer->forgetTile(tileX, tileZ);
        streamer->requestRect(ctx.cameraPos.x, ctx.cameraPos.z,
                              ctx.cameraPos.x, ctx.cameraPos.z);
    }
    if (result) {
        ImGui::Text("Region (%d, %d): %u lakes, %u rivers", tileX, tileZ,
                    static_cast<u32>(result->lakes.size()),
                    static_cast<u32>(result->rivers.size()));
        if (ImGui::Button("Accept -> records")) {
            accept(ctx);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(then Export)");
    }
    ImGui::TextDisabled("Retouch with the sculpt brushes; deltas stay a "
                        "separate layer.");

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
                             kMapTilesPerSide)) {
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

void TerrainGenTool::accept(const GenContext& ctx) {
    if (!result) {
        return;
    }
    const auto dir = platform::executableDir() / "data" / "mods" / "terrain";
    std::error_code errc;
    std::filesystem::create_directories(dir, errc);
    char name[64];
    std::snprintf(name, sizeof(name), "region_%d_%d.trg", tileX, tileZ);
    if (!world::writeTrgFile(dir / name, result->region)) {
        return;
    }
    const core::Guid assetGuid = regionAssetGuid(tileX, tileZ);
    ctx.levelEditor.addExportAsset(assetGuid, str { "terrain/" } + name);
    auto& session = ctx.levelEditor.editSession();

    // Region record: reuse-if-exists (by asset guid), else create.
    const reflect::TypeInfo& regionType =
        world::TerrainRegionForm::staticTypeInfo();
    core::Guid recordGuid {};
    data::forEach<world::TerrainRegionForm>(
        ctx.forms, [&](const world::TerrainRegionForm& form) {
            if (form.asset == assetGuid) {
                recordGuid = form.id;
            }
        });
    if (!recordGuid.isValid()) {
        char editorId[64];
        std::snprintf(editorId, sizeof(editorId), "GenRegion_%d_%d", tileX,
                      tileZ);
        recordGuid = session.createForm(regionType.id, editorId);
    }
    session.setField(recordGuid, regionType.findField("asset")->id,
                     reflect::Value { assetGuid });
    session.setField(recordGuid,
                     regionType.findField("detailAmplitude")->id,
                     reflect::Value { result->region.detailAmplitude });
    session.setField(recordGuid,
                     regionType.findField("detailWavelength")->id,
                     reflect::Value { result->region.detailWavelength });
    session.setField(recordGuid, regionType.findField("detailOctaves")->id,
                     reflect::Value { result->region.detailOctaves });

    // Water bodies as ordinary records (moddable in pure TOML).
    const reflect::TypeInfo& lakeType =
        world::WaterBodyForm::staticTypeInfo();
    for (size_t i = 0; i < result->lakes.size(); ++i) {
        const render::terraingen::Lake& lake = result->lakes[i];
        const core::Guid guid = derivedGuid(assetGuid, 0x1000 + i);
        if (!ctx.forms.find(guid)) {
            char editorId[64];
            std::snprintf(editorId, sizeof(editorId), "GenLake_%d_%d_%zu",
                          tileX, tileZ, i);
            session.createForm(lakeType.id, editorId, guid);
        }
        session.setField(guid, lakeType.findField("surfaceLevel")->id,
                         reflect::Value { lake.level });
        session.setField(guid, lakeType.findField("minX")->id,
                         reflect::Value { lake.minX });
        session.setField(guid, lakeType.findField("minZ")->id,
                         reflect::Value { lake.minZ });
        session.setField(guid, lakeType.findField("maxX")->id,
                         reflect::Value { lake.maxX });
        session.setField(guid, lakeType.findField("maxZ")->id,
                         reflect::Value { lake.maxZ });
    }
    const reflect::TypeInfo& riverType =
        world::RiverForm::staticTypeInfo();
    const reflect::TypeInfo& pointType =
        world::RiverPointForm::staticTypeInfo();
    for (size_t r = 0; r < result->rivers.size(); ++r) {
        const render::terraingen::River& river = result->rivers[r];
        const core::Guid riverGuid = derivedGuid(assetGuid, 0x2000 + r);
        if (!ctx.forms.find(riverGuid)) {
            char editorId[64];
            std::snprintf(editorId, sizeof(editorId), "GenRiver_%d_%d_%zu",
                          tileX, tileZ, r);
            session.createForm(riverType.id, editorId, riverGuid);
        }
        for (size_t p = 0; p < river.points.size(); ++p) {
            const render::terraingen::RiverPoint& pt = river.points[p];
            const core::Guid pointGuid =
                derivedGuid(riverGuid, 0x3000 + p);
            if (!ctx.forms.find(pointGuid)) {
                char editorId[80];
                std::snprintf(editorId, sizeof(editorId),
                              "GenRiverPt_%zu_%zu", r, p);
                session.createForm(pointType.id, editorId, pointGuid);
            }
            session.setField(pointGuid, pointType.findField("parent")->id,
                             reflect::Value { riverGuid });
            session.setField(pointGuid, pointType.findField("index")->id,
                             reflect::Value { static_cast<i32>(p) });
            session.setField(
                pointGuid, pointType.findField("position")->id,
                reflect::Value { Vec3 { pt.x, pt.surface, pt.z } });
            session.setField(pointGuid,
                             pointType.findField("halfWidth")->id,
                             reflect::Value { pt.halfWidth });
        }
    }
    LOG_INFO("Terrain gen: region ({}, {}) staged ({} lakes, {} rivers) — "
             "Export writes the mod",
             tileX, tileZ, result->lakes.size(), result->rivers.size());
}

} // namespace game
