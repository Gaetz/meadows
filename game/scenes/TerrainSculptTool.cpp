#include "game/scenes/TerrainSculptTool.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <system_error>

#include "engine/render/landscape/TerrainSystem.hpp" // kChunkSize

#include <glm/glm.hpp>
#include <imgui.h>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Paths.hpp"
#include "game/LevelEditor.hpp"
#include "world/terrain/TerrainPatches.hpp" // world::writeTerFile
#include "world/worldspace/WorldForms.hpp"  // world::TerrainPatchForm

namespace game {

void TerrainSculptTool::drawPanel(const SculptContext& ctx) {
    ImGui::Checkbox("Sculpt terrain", &mode);
    if (mode) {
        ImGui::Combo("Brush", &brushKind, "Raise\0Lower\0Flatten\0Smooth\0");
        ImGui::SliderFloat("Radius (m)", &brushRadius, 1.0f, 24.0f, "%.0f");
        ImGui::SliderFloat("Strength", &brushStrength, 0.2f, 10.0f, "%.1f");
        ImGui::Text("Sculpted chunks: %u", static_cast<u32>(grids.size()));
        if (ImGui::Button("Save terrain to mod")) {
            saveToMod(ctx);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(then Export)");
    }
}

void TerrainSculptTool::stroke(const SculptContext& ctx, const Vec3& ground,
                               f32 dt) {
    // [cpp-tuning] ~20 Hz live re-mesh cadence (editor plumbing, not feel).
    constexpr f32 kPreviewInterval = 0.05f;
    if (!strokeActive) {
        strokeActive = true;
        flattenTarget = ground.y;         // grabbed at stroke start
        previewTimer = kPreviewInterval;  // preview on the very first frame
    }
    applyBrush(ctx, ground, dt);
    // Live preview: re-mesh the touched terrain in place as the brush moves
    // (throttled; the terrain swaps seamlessly). The heavy commit — collision,
    // cell snap, grass/veg re-scatter — waits for the stroke's release.
    previewTimer += dt;
    if (previewTimer >= kPreviewInterval) {
        previewTimer = 0.0f;
        publish(ctx, /*commit=*/false);
    }
}

void TerrainSculptTool::endStroke(const SculptContext& ctx) {
    if (!strokeActive) {
        return;
    }
    strokeActive = false;
    publish(ctx, /*commit=*/true); // the permanent publish, once per stroke
}

void TerrainSculptTool::applyBrush(const SculptContext& ctx, const Vec3& center,
                                   f32 dt) {
    world::BrushParams brush;
    switch (brushKind) {
    case 1: brush.kind = world::BrushKind::Lower; break;
    case 2: brush.kind = world::BrushKind::Flatten; break;
    case 3: brush.kind = world::BrushKind::Smooth; break;
    default: brush.kind = world::BrushKind::Raise; break;
    }
    brush.radius = brushRadius;
    brush.strength = brushStrength;
    brush.flattenTarget = flattenTarget;
    const render::TerrainParams& params = ctx.terrainParams;
    world::applyTerrainBrush(
        grids, ctx.publishedPatches, render::TerrainSystem::kChunkSize, brush,
        { center.x, center.z }, dt,
        [&params](f32 x, f32 z) { return render::terrain::height(params, x, z); });
}

void TerrainSculptTool::publish(const SculptContext& ctx, bool commit) {
    if (grids.empty()) {
        return;
    }
    // New immutable overlay = published chunks overridden by the working grids;
    // in-flight workers keep the old instance alive through their copied
    // TerrainParams (shared_ptr). The scene swaps it in and rebuilds only
    // the changed chunks.
    std::vector<u64> changed;
    auto next = world::publishBrushGrids(grids, ctx.publishedPatches,
                                         render::TerrainSystem::kChunkSize,
                                         changed);
    ctx.republishTerrain(std::move(next), changed, commit);
}

void TerrainSculptTool::saveToMod(const SculptContext& ctx) {
    const auto dir = platform::executableDir() / "data" / "mods" / "terrain";
    std::error_code errc;
    std::filesystem::create_directories(dir, errc);
    const reflect::TypeInfo& type = world::TerrainPatchForm::staticTypeInfo();
    for (const auto& [key, grid] : grids) {
        const i32 cx = static_cast<i32>(key >> 32);
        const i32 cz = static_cast<i32>(key & 0xffffffffu);
        char name[64];
        std::snprintf(name, sizeof(name), "patch_%d_%d.ter", cx, cz);
        if (!world::writeTerFile(dir / name, grid)) {
            continue;
        }
        // Deterministic asset guid per chunk (stable across saves).
        char guidText[40];
        std::snprintf(guidText, sizeof(guidText),
                      "7e88a110-0000-4000-8000-%012llx",
                      static_cast<unsigned long long>(key & 0xFFFFFFFFFFFFull));
        const core::Guid assetGuid = *core::Guid::fromString(guidText);
        ctx.levelEditor.addExportAsset(assetGuid, str { "terrain/" } + name);
        // One TerrainPatchForm per chunk — reuse the existing record if this
        // chunk was already authored (patch it), else create.
        core::Guid recordGuid {};
        data::forEach<world::TerrainPatchForm>(
            ctx.forms, [&](const world::TerrainPatchForm& form) {
                if (form.chunkX == cx && form.chunkZ == cz) {
                    recordGuid = form.id;
                }
            });
        auto& session = ctx.levelEditor.editSession();
        if (!recordGuid.isValid()) {
            char editorId[64];
            std::snprintf(editorId, sizeof(editorId), "SculptPatch_%d_%d", cx,
                          cz);
            recordGuid = session.createForm(type.id, editorId);
            session.setField(recordGuid, type.findField("chunkX")->id,
                             reflect::Value { cx });
            session.setField(recordGuid, type.findField("chunkZ")->id,
                             reflect::Value { cz });
        }
        session.setField(recordGuid, type.findField("asset")->id,
                         reflect::Value { assetGuid });
    }
    LOG_INFO("{} sculpted chunk(s) staged — Export writes the mod",
             grids.size());
}

} // namespace game
