#include "game/scenes/TerrainGenTool.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>

#include <imgui.h>

#include "data/forms/FormQuery.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Paths.hpp"
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
    // Land finished bakes (worker mailbox, same pattern as the streamer).
    TileBakeResult baked;
    while (done->tryPop(baked)) {
        baking = false;
        result = std::move(baked);
        if (ctx.publishPreview) {
            TileBakeResult copy = *result;
            ctx.publishPreview(std::move(copy), tileX, tileZ);
        }
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
        TileBakeParams params;
        params.worldSeed = seed;
        params.tileSize = regionSize;
        const auto work = [params, tx = tileX, tz = tileZ,
                           queue = done] {
            queue->push(render::terraingen::bakeTile(params, tx, tz));
        };
        if (ctx.jobs) {
            ctx.jobs->enqueue(work);
        } else {
            work();
        }
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
