#include "game/scenes/TerrainSculptTool.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <system_error>

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
    constexpr f32 kPreviewInterval = 0.05f; // ~20 Hz live re-mesh
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

render::HeightPatch& TerrainSculptTool::gridFor(const SculptContext& ctx,
                                                i32 cx, i32 cz) {
    const u64 key = render::HeightPatches::keyOf(cx, cz);
    const auto it = grids.find(key);
    if (it != grids.end()) {
        return it->second;
    }
    // Seed from the published overlay when the chunk is already authored.
    if (ctx.publishedPatches) {
        if (const auto existing = ctx.publishedPatches->chunks.find(key);
            existing != ctx.publishedPatches->chunks.end()) {
            return grids.emplace(key, existing->second).first->second;
        }
    }
    render::HeightPatch fresh;
    fresh.samples = 65;
    fresh.deltas.assign(65 * 65, 0.0f);
    return grids.emplace(key, std::move(fresh)).first->second;
}

void TerrainSculptTool::applyBrush(const SculptContext& ctx, const Vec3& center,
                                   f32 dt) {
    constexpr f32 kChunk = 64.0f;
    const i32 minCx =
        static_cast<i32>(std::floor((center.x - brushRadius) / kChunk));
    const i32 maxCx =
        static_cast<i32>(std::floor((center.x + brushRadius) / kChunk));
    const i32 minCz =
        static_cast<i32>(std::floor((center.z - brushRadius) / kChunk));
    const i32 maxCz =
        static_cast<i32>(std::floor((center.z + brushRadius) / kChunk));
    for (i32 cz = minCz; cz <= maxCz; ++cz) {
        for (i32 cx = minCx; cx <= maxCx; ++cx) {
            render::HeightPatch& grid = gridFor(ctx, cx, cz);
            for (u32 row = 0; row < grid.samples; ++row) {
                for (u32 col = 0; col < grid.samples; ++col) {
                    const f32 x =
                        static_cast<f32>(cx) * kChunk + static_cast<f32>(col);
                    const f32 z =
                        static_cast<f32>(cz) * kChunk + static_cast<f32>(row);
                    const f32 dx = x - center.x;
                    const f32 dz = z - center.z;
                    const f32 dist = std::sqrt(dx * dx + dz * dz);
                    if (dist >= brushRadius) {
                        continue;
                    }
                    const f32 t = 1.0f - dist / brushRadius;
                    const f32 falloff = t * t * (3.0f - 2.0f * t);
                    f32& delta = grid.deltas[row * grid.samples + col];
                    switch (brushKind) {
                    case 0: // raise
                        delta += brushStrength * falloff * dt;
                        break;
                    case 1: // lower
                        delta -= brushStrength * falloff * dt;
                        break;
                    case 2: { // flatten toward the stroke-start height:
                        // work against the LIVE height (base + published
                        // patch); the working delta absorbs the gap.
                        const f32 current =
                            render::terrain::height(ctx.terrainParams, x, z);
                        const f32 gap = flattenTarget - current;
                        delta += gap * glm::min(2.5f * falloff * dt, 1.0f);
                        break;
                    }
                    case 3: { // smooth: relax toward the neighbour average
                        const u32 c0 = col > 0 ? col - 1 : col;
                        const u32 c1 = glm::min(col + 1, grid.samples - 1);
                        const u32 r0 = row > 0 ? row - 1 : row;
                        const u32 r1 = glm::min(row + 1, grid.samples - 1);
                        const f32 average =
                            (grid.deltas[row * grid.samples + c0] +
                             grid.deltas[row * grid.samples + c1] +
                             grid.deltas[r0 * grid.samples + col] +
                             grid.deltas[r1 * grid.samples + col]) *
                            0.25f;
                        delta += (average - delta) *
                                 glm::min(4.0f * falloff * dt, 1.0f);
                        break;
                    }
                    default:
                        break;
                    }
                }
            }
        }
    }
}

void TerrainSculptTool::publish(const SculptContext& ctx, bool commit) {
    if (grids.empty()) {
        return;
    }
    // New immutable overlay = published chunks overridden by the working grids;
    // in-flight workers keep the old instance alive through their copied
    // TerrainParams (shared_ptr). The scene swaps it in and rebuilds.
    auto next = std::make_shared<render::HeightPatches>();
    next->chunkSize = 64.0f;
    if (ctx.publishedPatches) {
        next->chunks = ctx.publishedPatches->chunks;
    }
    std::vector<u64> changed;
    changed.reserve(grids.size());
    for (const auto& [key, grid] : grids) {
        next->chunks[key] = grid;
        changed.push_back(key); // only these chunks need a rebuild
    }
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
    LOG_INFO("B9: {} sculpted chunk(s) staged — Export writes the mod",
             grids.size());
}

} // namespace game
